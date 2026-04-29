from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import math
import re
import secrets
import shutil
import struct
import time
from collections.abc import Awaitable, Callable
from typing import Any

from .hermes import HermesResponsesClient
from .models import DataChannelMessage, DebugPlaybackRequest, DeviceCommandRequest, SpeechTurn
from .registry import DeviceSession, InMemoryRegistry
from .speech import SpeechRuntime
from .spoken_text import sanitize_spoken_text

logger = logging.getLogger(__name__)


NativeTurnHandler = Callable[[DeviceSession, SpeechTurn, dict[str, Any] | None], Awaitable[str | None]]
DeviceVolumeHandler = Callable[[DeviceSession, int], Awaitable[None]]


def _format_ms(seconds: float | None) -> str:
    if seconds is None:
        return "unknown"
    return f"{seconds * 1000:.0f}ms"


class GatewaySessionManager:
    def __init__(
        self,
        registry: InMemoryRegistry,
        hermes: HermesResponsesClient,
        speech: SpeechRuntime,
        *,
        conversation_mode: str = "device",
        wake_word_enabled: bool = False,
        wake_word: str = "hey willow",
        sleep_timeout_seconds: float = 30.0,
        on_device_volume: DeviceVolumeHandler | None = None,
    ) -> None:
        self._registry = registry
        self._hermes = hermes
        self._speech = speech
        self._conversation_mode = conversation_mode
        self._wake_word_enabled = wake_word_enabled
        self._wake_word = wake_word.strip() or "hey willow"
        self._sleep_timeout_seconds = max(0.1, float(sleep_timeout_seconds))
        self._on_device_volume = on_device_volume
        self._sleep_tasks: dict[str, asyncio.Task[None]] = {}

    async def create_session(self, device_id: str) -> DeviceSession:
        existing = await self._registry.get_session_for_device(device_id)
        if existing:
            if existing.active_turn and not existing.active_turn.done():
                existing.active_turn.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await existing.active_turn
            existing.connected = True
            existing.sender = None
            existing.peer = None
            await self._registry.upsert_session(existing)
            return existing
        record = await self._registry.get_device(device_id)
        session_id = secrets.token_urlsafe(12)
        session = DeviceSession(
            session_id=session_id,
            device_id=device_id,
            conversation=self._conversation_for(device_id, session_id, record.conversation if record else None),
        )
        return await self._registry.upsert_session(session)

    async def bind_sender(self, session_id: str, sender) -> DeviceSession | None:
        session = await self._registry.bind_sender(session_id, sender)
        return session

    async def interrupt(self, device_id: str, reason: str) -> bool:
        session = await self._registry.get_session_for_device(device_id)
        if not session:
            return False
        if session.active_turn and not session.active_turn.done():
            session.active_turn.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await session.active_turn
        await self._set_assistant_state(session, "idle")
        await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "idle", "reason": reason}))
        if self._should_resume_after_interrupt(session, reason):
            session.capturing_audio = True
            await self._set_assistant_state(session, "listening")
            await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "listening"}))
        return True

    async def end_conversation(self, device_id: str, reason: str = "tool") -> dict[str, Any]:
        session = await self._registry.get_session_for_device(device_id)
        if not session or not session.connected:
            return {"accepted": False, "reason": "device is not connected"}
        session.device_state["end_conversation_requested"] = True
        session.device_state["end_conversation_reason"] = reason
        session.capturing_audio = False
        await self._speech.pause_audio_ingest(session)
        await self._registry.upsert_session(session)
        if session.active_turn and not session.active_turn.done():
            return {"accepted": True, "status": "ending_after_turn", "device_id": device_id}
        await self._close_conversation_session(session, reason=reason)
        return {"accepted": True, "status": "ended", "device_id": device_id}

    async def attach_audio_track(self, session_id: str, track) -> None:
        session = await self._registry.get_session(session_id)
        if not session:
            return
        if self._media_mode_active(session):
            await self._apply_media_mode_policy(session, reason="audio_attached")
        else:
            session.capturing_audio = True
            await self._send_listening_or_sleeping(session, reason="audio_attached")
        await self._speech.attach_audio_track(
            session=session,
            track=track,
            emit=self._send,
            on_turn=self._submit_turn,
            on_interrupt=self._handle_speech_interrupt,
        )

    async def attach_output_track(self, session_id: str, push_audio) -> None:
        session = await self._registry.get_session(session_id)
        if not session:
            return
        await self._speech.attach_output_track(session=session, push_audio=push_audio)

    async def detach_session(self, session_id: str) -> None:
        session = await self._registry.get_session(session_id)
        if not session:
            return
        session.capturing_audio = False
        self._cancel_sleep_timer(session)
        await self._speech.detach_session(session)

    async def handle_control_message(self, session_id: str, message: dict[str, Any]) -> None:
        session = await self._registry.get_session(session_id)
        if not session:
            return
        msg_type = message.get("type")
        payload = message.get("payload", {})
        if msg_type == "hello":
            session.device_state["hello"] = payload
            if "capabilities" in payload:
                session.device_state["capabilities"] = payload["capabilities"]
            await self._registry.upsert_session(session)
            if payload.get("wake_detected"):
                session.device_state["wake_detected"] = True
                if payload.get("media_barge_in"):
                    session.device_state["media_barge_in"] = True
                session.capturing_audio = True
                session.device_state["media_capture_reenabled_at"] = time.monotonic()
                await self._speech.resume_audio_ingest(session)
                await self._set_assistant_state(session, "listening")
                await self._send(
                    session,
                    DataChannelMessage(
                        type="assistant.state",
                        payload={
                            "state": "listening",
                            "reason": "local_wake",
                            "session_id": session.session_id,
                            "conversation": session.conversation,
                        },
                    ),
                )
            elif self._media_mode_active(session):
                await self._apply_media_mode_policy(
                    session,
                    reason="hello",
                    payload={"session_id": session.session_id, "conversation": session.conversation},
                )
            else:
                session.capturing_audio = True
                await self._send_listening_or_sleeping(
                    session,
                    reason="hello",
                    payload={"session_id": session.session_id, "conversation": session.conversation},
                )
            return
        if msg_type == "interrupt":
            await self.interrupt(session.device_id, payload.get("reason", "device"))
            return
        if msg_type == "device.state":
            session.device_state.update(payload)
            if payload.get("wake_detected"):
                session.capturing_audio = True
                session.device_state["media_capture_reenabled_at"] = time.monotonic()
                await self._speech.resume_audio_ingest(session)
                await self._set_assistant_state(session, "listening")
                await self._send(
                    session,
                    DataChannelMessage(type="assistant.state", payload={"state": "listening", "reason": "local_wake"}),
                )
            await self._registry.upsert_session(session)
            return
        if msg_type == "audio.stats":
            session.device_state["audio_stats"] = payload
            await self._registry.upsert_session(session)
            return
        if msg_type == "audio.aec_probe":
            session.device_state["audio_aec_probe"] = payload
            await self._registry.upsert_session(session)
            logger.info("AEC probe result for %s: %s", session.device_id, payload)
            return
        if msg_type == "mute.set":
            session.device_state["muted"] = bool(payload.get("muted", False))
            await self._registry.upsert_session(session)
            return
        if msg_type == "volume.set":
            if "volume" in payload:
                try:
                    volume = max(0, min(int(payload["volume"]), 100))
                except (TypeError, ValueError):
                    logger.warning("Ignoring invalid volume payload for %s: %r", session.device_id, payload.get("volume"))
                    return
                session.device_state["volume"] = volume
                if self._on_device_volume is not None:
                    try:
                        await self._on_device_volume(session, volume)
                    except Exception:
                        logger.exception("Device volume handler failed for %s", session.device_id)
            await self._registry.upsert_session(session)
            return
        if msg_type == "debug.user_text":
            turn = await self._speech.transcribe_text_payload(payload.get("text", ""))
            if turn is not None:
                await self._submit_turn(session, turn, payload.get("hello"))

    async def send_command(self, device_id: str, request: DeviceCommandRequest) -> bool:
        session = await self._registry.get_session_for_device(device_id)
        if not session or not session.connected:
            return False
        await self._send(session, DataChannelMessage(type="device.command", payload={"type": request.type, **request.payload}))
        return True

    async def debug_playback(self, device_id: str, request: DebugPlaybackRequest) -> dict[str, Any]:
        session = await self._registry.get_session_for_device(device_id)
        if not session or not session.connected or session.output_track is None:
            return {"accepted": False, "reason": "device output track is not connected"}

        pcm = await self._render_debug_pcm(request)
        if not pcm:
            return {"accepted": False, "reason": "no audio rendered"}

        await self._set_assistant_state(session, "speaking")
        await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "speaking", "debug": True}))
        chunk_bytes = max(1, request.sample_rate_hz // 100) * 2
        for offset in range(0, len(pcm), chunk_bytes):
            await session.output_track.push_pcm(pcm[offset : offset + chunk_bytes])
        await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "listening", "debug": True}))
        await self._set_assistant_state(session, "listening")
        return {
            "accepted": True,
            "kind": request.kind,
            "bytes": len(pcm),
            "sample_rate_hz": request.sample_rate_hz,
        }

    async def _render_debug_pcm(self, request: DebugPlaybackRequest) -> bytes:
        if request.kind == "tone":
            sample_count = max(1, int(request.sample_rate_hz * request.duration_ms / 1000))
            amplitude = int(32767 * max(0.0, min(request.gain, 1.0)))
            return b"".join(
                struct.pack(
                    "<h",
                    int(amplitude * math.sin(2.0 * math.pi * request.frequency_hz * sample / request.sample_rate_hz)),
                )
                for sample in range(sample_count)
            )

        if not request.path:
            return b""
        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg is None:
            raise RuntimeError("ffmpeg is required for debug file playback")
        process = await asyncio.create_subprocess_exec(
            ffmpeg,
            "-v",
            "error",
            "-i",
            request.path,
            "-ac",
            "1",
            "-ar",
            str(request.sample_rate_hz),
            "-f",
            "s16le",
            "pipe:1",
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        stdout, stderr = await process.communicate()
        if process.returncode != 0:
            raise RuntimeError(stderr.decode("utf-8", "replace").strip() or "ffmpeg failed")
        if request.gain != 1.0:
            stdout = self._apply_gain(stdout, request.gain)
        return stdout

    @staticmethod
    def _apply_gain(pcm: bytes, gain: float) -> bytes:
        gain = max(0.0, min(gain, 1.0))
        output = bytearray(len(pcm))
        for index in range(0, len(pcm) - 1, 2):
            sample = struct.unpack_from("<h", pcm, index)[0]
            struct.pack_into("<h", output, index, int(sample * gain))
        return bytes(output)

    async def _run_turn(self, session: DeviceSession, turn: SpeechTurn, hello_metadata: dict[str, Any] | None) -> None:
        turn_started_at = time.monotonic()
        first_text_at: float | None = None
        response_created_at: float | None = None
        completed_at: float | None = None
        latency: dict[str, Any] = {
            "hermes_submit_at": turn_started_at,
            "source": turn.source,
            "chars": len(turn.text),
        }
        for key in ("flux_start_at", "flux_first_transcript_at", "flux_end_at"):
            if key in turn.metadata:
                latency[key] = turn.metadata[key]
        session.device_state["_latency_turn"] = latency
        try:
            session.capturing_audio = False
            session.device_state["last_assistant_text"] = ""
            await self._set_assistant_state(session, "thinking")
            await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "thinking"}))
            logger.info(
                "Submitting Hermes turn for %s conversation=%s source=%s chars=%s",
                session.device_id,
                session.conversation,
                turn.source,
                len(turn.text),
            )
            async for event in self._hermes.stream_text_turn(
                device_id=session.device_id,
                conversation=session.conversation,
                text=turn.text,
                hello_metadata=hello_metadata,
            ):
                if event.kind == "response.created":
                    response_created_at = time.monotonic()
                    latency["hermes_response_created_at"] = response_created_at
                    logger.info(
                        "LATENCY %s hermes_response_created after_submit=%s after_flux_eot=%s",
                        session.device_id,
                        _format_ms(response_created_at - turn_started_at),
                        _format_ms(response_created_at - latency["flux_end_at"])
                        if isinstance(latency.get("flux_end_at"), (int, float))
                        else "unknown",
                    )
                    continue
                if event.kind == "assistant.text.delta":
                    delta_text = sanitize_spoken_text(event.payload.get("text", ""), strip=False)
                    await self._send(
                        session,
                        DataChannelMessage(type="assistant.text.delta", payload={"text": delta_text}),
                    )
                    if self._media_mode_active(session):
                        if delta_text:
                            logger.info(
                                "Suppressing assistant speech while media is playing for %s: chars=%s",
                                session.device_id,
                                len(delta_text),
                            )
                        continue
                    if delta_text:
                        if first_text_at is None:
                            first_text_at = time.monotonic()
                            latency["hermes_first_text_at"] = first_text_at
                            logger.info(
                                "LATENCY %s hermes_first_text after_submit=%s after_response_created=%s after_flux_eot=%s",
                                session.device_id,
                                _format_ms(first_text_at - turn_started_at),
                                _format_ms(first_text_at - response_created_at)
                                if response_created_at is not None
                                else "unknown",
                                _format_ms(first_text_at - latency["flux_end_at"])
                                if isinstance(latency.get("flux_end_at"), (int, float))
                                else "unknown",
                            )
                        session.device_state["last_assistant_text"] = (
                            str(session.device_state.get("last_assistant_text", "")) + delta_text
                        )[-2000:]
                        if session.assistant_state != "speaking":
                            await self._set_assistant_state(session, "speaking")
                            await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "speaking"}))
                        await self._speech.on_text_delta(session, delta_text)
                elif event.kind == "tool.call":
                    await self._set_assistant_state(session, "tool")
                    await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "tool"}))
                    await self._send(
                        session,
                        DataChannelMessage(
                            type="tool.progress",
                            payload={"phase": "call", **event.payload},
                        ),
                    )
                elif event.kind == "tool.output":
                    await self._send(
                        session,
                        DataChannelMessage(
                            type="tool.progress",
                            payload={"phase": "output", **event.payload},
                        ),
                    )
                elif event.kind == "response.completed":
                    completed_at = time.monotonic()
                    latency["hermes_completed_at"] = completed_at
                    logger.info(
                        "LATENCY %s hermes_completed after_submit=%s after_first_text=%s",
                        session.device_id,
                        _format_ms(completed_at - turn_started_at),
                        _format_ms(completed_at - first_text_at)
                        if first_text_at is not None
                        else "unknown",
                    )
                    await self._speech.on_turn_complete(session)
                    await self._wait_for_output_playout(session, latency)
                    self._log_latency_summary(session, latency)
                    session.device_state["last_assistant_speech_ended_at"] = time.monotonic()
                    await self._resume_listening(session)
                elif event.kind == "response.failed":
                    await self._send(
                        session,
                        DataChannelMessage(type="error", payload={"message": json.dumps(event.payload)}),
                    )
                    await self._resume_listening(session)
        except asyncio.CancelledError:
            logger.info("Cancelled active turn for %s", session.device_id)
            raise
        except Exception as exc:
            logger.exception("Text turn failed")
            await self._send(session, DataChannelMessage(type="error", payload={"message": str(exc)}))
            await self._resume_listening(session)
        finally:
            if session.connected:
                if self._media_mode_active(session):
                    session.capturing_audio = False
                    await self._set_assistant_state(session, "idle")
                else:
                    session.capturing_audio = True
                    await self._set_assistant_state(session, "listening")
            else:
                await self._set_assistant_state(session, "idle")

    def _log_latency_summary(self, session: DeviceSession, latency: dict[str, Any]) -> None:
        flux_start_at = latency.get("flux_start_at")
        flux_first_transcript_at = latency.get("flux_first_transcript_at")
        flux_end_at = latency.get("flux_end_at")
        hermes_submit_at = latency.get("hermes_submit_at")
        hermes_response_created_at = latency.get("hermes_response_created_at")
        hermes_first_text_at = latency.get("hermes_first_text_at")
        hermes_completed_at = latency.get("hermes_completed_at")
        aura_first_text_at = latency.get("aura_first_text_at")
        aura_first_audio_at = latency.get("aura_first_audio_at")
        aura_drain_at = latency.get("aura_drain_at")

        def delta(later: Any, earlier: Any) -> str:
            if isinstance(later, (int, float)) and isinstance(earlier, (int, float)):
                return _format_ms(later - earlier)
            return "unknown"

        logger.info(
            "LATENCY_SUMMARY %s chars=%s flux_start_to_first_text=%s flux_start_to_eot=%s "
            "flux_first_transcript_to_eot=%s eot_to_submit=%s submit_to_response_created=%s "
            "submit_to_first_text=%s response_created_to_first_text=%s first_text_to_aura_audio=%s "
            "first_audio_to_drain=%s total_start_to_drain=%s",
            session.device_id,
            latency.get("chars", "unknown"),
            delta(flux_first_transcript_at, flux_start_at),
            delta(flux_end_at, flux_start_at),
            delta(flux_end_at, flux_first_transcript_at),
            delta(hermes_submit_at, flux_end_at),
            delta(hermes_response_created_at, hermes_submit_at),
            delta(hermes_first_text_at, hermes_submit_at),
            delta(hermes_first_text_at, hermes_response_created_at),
            delta(aura_first_audio_at, hermes_first_text_at),
            delta(aura_drain_at, aura_first_audio_at),
            delta(aura_drain_at, flux_start_at),
            )

    async def _wait_for_output_playout(self, session: DeviceSession, latency: dict[str, Any]) -> None:
        output_track = session.output_track
        wait_for_playout_drain = getattr(output_track, "wait_for_playout_drain", None)
        if not callable(wait_for_playout_drain):
            return
        before_wait = time.monotonic()
        await wait_for_playout_drain()
        drained_at = time.monotonic()
        latency["output_playout_drain_at"] = drained_at
        logger.info(
            "LATENCY %s output_playout_drain after_aura_drain=%s wait=%s",
            session.device_id,
            _format_ms(drained_at - latency["aura_drain_at"])
            if isinstance(latency.get("aura_drain_at"), (int, float))
            else "unknown",
            _format_ms(drained_at - before_wait),
        )

    async def _send(self, session: DeviceSession, message: DataChannelMessage) -> None:
        if session.sender is None:
            return
        await session.sender(message.model_dump(mode="json"))

    async def _send_listening_or_sleeping(
        self,
        session: DeviceSession,
        *,
        reason: str,
        payload: dict[str, Any] | None = None,
    ) -> None:
        base_payload = payload or {}
        if self._media_mode_active(session):
            await self._apply_media_mode_policy(session, reason=reason, payload=base_payload)
            return
        if self._wake_word_enabled and not self._is_awake(session):
            await self._set_sleeping(session, reason=reason, payload=base_payload)
            return
        await self._set_assistant_state(session, "listening")
        await self._send(
            session,
            DataChannelMessage(type="assistant.state", payload={"state": "listening", "reason": reason, **base_payload}),
        )

    async def _set_sleeping(
        self,
        session: DeviceSession,
        *,
        reason: str,
        payload: dict[str, Any] | None = None,
    ) -> None:
        session.device_state["awake"] = False
        await self._set_assistant_state(session, "sleeping")
        await self._send(
            session,
            DataChannelMessage(
                type="assistant.state",
                payload={"state": "sleeping", "reason": reason, "wake_word": self._wake_word, **(payload or {})},
            ),
        )

    async def _set_assistant_state(self, session: DeviceSession, state: str) -> None:
        session.assistant_state = state
        await self._registry.upsert_session(session)

    def _wake_pattern(self) -> re.Pattern[str]:
        escaped_words = [re.escape(part) for part in self._wake_word.lower().split()]
        return re.compile(r"\b" + r"[\s,.\-!?:;]+".join(escaped_words) + r"\b", re.IGNORECASE)

    def _strip_wake_word(self, text: str) -> tuple[bool, str]:
        match = self._wake_pattern().search(text)
        if not match:
            return False, text
        stripped = f"{text[:match.start()]} {text[match.end():]}"
        stripped = re.sub(r"\s+", " ", stripped).strip(" ,.!?:;-")
        return True, stripped

    def _is_awake(self, session: DeviceSession) -> bool:
        if not self._wake_word_enabled:
            return True
        awake_until = session.device_state.get("awake_until_monotonic")
        return isinstance(awake_until, (int, float)) and time.monotonic() < float(awake_until)

    async def _mark_awake(self, session: DeviceSession, *, reason: str, emit: bool = False) -> None:
        session.device_state["awake"] = True
        session.device_state["awake_until_monotonic"] = time.monotonic() + self._sleep_timeout_seconds
        await self._registry.upsert_session(session)
        self._schedule_sleep_timer(session)
        if emit:
            await self._set_assistant_state(session, "listening")
            await self._send(
                session,
                DataChannelMessage(
                    type="assistant.state",
                    payload={
                        "state": "listening",
                        "reason": reason,
                        "sleep_timeout_seconds": self._sleep_timeout_seconds,
                    },
                ),
            )

    def _schedule_sleep_timer(self, session: DeviceSession) -> None:
        self._cancel_sleep_timer(session)
        self._sleep_tasks[session.session_id] = asyncio.create_task(self._sleep_after_inactivity(session.session_id))

    def _cancel_sleep_timer(self, session: DeviceSession) -> None:
        task = self._sleep_tasks.pop(session.session_id, None)
        if task and not task.done():
            task.cancel()

    async def _sleep_after_inactivity(self, session_id: str) -> None:
        try:
            while True:
                session = await self._registry.get_session(session_id)
                if session is None or not session.connected:
                    return
                awake_until = session.device_state.get("awake_until_monotonic")
                if not isinstance(awake_until, (int, float)):
                    return
                delay = float(awake_until) - time.monotonic()
                if delay > 0:
                    await asyncio.sleep(delay)
                    continue
                if session.active_turn and not session.active_turn.done():
                    await asyncio.sleep(0.25)
                    continue
                await self._set_sleeping(session, reason="inactivity_timeout")
                return
        except asyncio.CancelledError:
            raise

    async def _submit_turn(
        self,
        session: DeviceSession,
        turn: SpeechTurn,
        hello_metadata: dict[str, Any] | None = None,
    ) -> None:
        if self._wake_word_enabled:
            if self._is_awake(session):
                await self._mark_awake(session, reason="activity")
            else:
                woke, stripped_text = self._strip_wake_word(turn.text)
                if not woke:
                    logger.info("Ignoring sleeping-device transcript for %s: %s", session.device_id, turn.text)
                    await self._set_sleeping(session, reason="ambient_ignored")
                    return
                await self._mark_awake(session, reason="wake_word", emit=True)
                if not stripped_text:
                    logger.info("Wake word only for %s; waiting for follow-up", session.device_id)
                    return
                turn = turn.model_copy(update={"text": stripped_text})

        if session.active_turn and not session.active_turn.done():
            if session.assistant_state in {"thinking", "speaking", "tool"} or self._media_mode_active(session):
                logger.info(
                    "Superseding active turn for %s while assistant_state=%s",
                    session.device_id,
                    session.assistant_state,
                )
                await self.interrupt(session.device_id, "superseded")
            else:
                logger.info(
                    "Ignoring overlapping turn for %s while assistant_state=%s",
                    session.device_id,
                    session.assistant_state,
                )
                return
        session.active_turn = asyncio.create_task(self._run_turn(session, turn, hello_metadata))

    async def _handle_speech_interrupt(self, session: DeviceSession, reason: str) -> None:
        await self.interrupt(session.device_id, reason)

    async def _resume_listening(self, session: DeviceSession) -> None:
        if session.device_state.get("end_conversation_requested"):
            reason = str(session.device_state.get("end_conversation_reason") or "tool")
            await self._close_conversation_session(session, reason=reason)
            return
        if self._media_mode_active(session):
            await self._apply_media_mode_policy(session, reason="media_playing")
            return
        if self._wake_word_enabled:
            await self._mark_awake(session, reason="assistant_complete")
        await self._set_assistant_state(session, "idle")
        await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "idle"}))
        session.capturing_audio = True
        await self._send_listening_or_sleeping(session, reason="resume")

    @staticmethod
    def _media_mode_active(session: DeviceSession) -> bool:
        return bool(session.device_state.get("media_playing"))

    async def _apply_media_mode_policy(
        self,
        session: DeviceSession,
        *,
        reason: str,
        payload: dict[str, Any] | None = None,
    ) -> None:
        session.capturing_audio = False
        session.device_state.pop("wake_detected", None)
        session.device_state.pop("media_barge_in", None)
        session.device_state.pop("media_capture_reenabled_at", None)
        await self._speech.pause_audio_ingest(session)
        await self._set_assistant_state(session, "idle")
        await self._send(
            session,
            DataChannelMessage(type="device.command", payload={"type": "media.mode", "playing": True}),
        )
        await self._send(
            session,
            DataChannelMessage(
                type="assistant.state",
                payload={"state": "idle", "reason": reason, "media_playing": True, **(payload or {})},
            ),
        )

    async def _close_conversation_session(self, session: DeviceSession, *, reason: str) -> None:
        self._cancel_sleep_timer(session)
        session.device_state["awake"] = False
        session.device_state.pop("awake_until_monotonic", None)
        session.device_state.pop("end_conversation_requested", None)
        session.device_state.pop("end_conversation_reason", None)
        session.capturing_audio = False
        await self._speech.pause_audio_ingest(session)
        await self._set_assistant_state(session, "idle")
        await self._send(
            session,
            DataChannelMessage(type="assistant.state", payload={"state": "idle", "reason": reason}),
        )
        await self._send(
            session,
            DataChannelMessage(type="device.command", payload={"type": "end_conversation", "reason": reason}),
        )
        peer = session.peer
        session.connected = False
        await self._registry.upsert_session(session)
        await self.detach_session(session.session_id)
        if peer is not None:
            with contextlib.suppress(Exception):
                await peer.close()

    @staticmethod
    def _should_resume_after_interrupt(session: DeviceSession, reason: str) -> bool:
        return session.connected and reason not in {"failed", "closed", "disconnected", "superseded"}

    def _conversation_for(self, device_id: str, session_id: str, device_conversation: str | None) -> str:
        if self._conversation_mode == "session":
            return f"iot:{device_id}:session:{session_id}"
        return device_conversation or f"iot:{device_id}"


class NativeGatewaySessionManager(GatewaySessionManager):
    """Route device speech turns through an in-process Hermes gateway handler."""

    def __init__(
        self,
        registry: InMemoryRegistry,
        speech: SpeechRuntime,
        handle_turn: NativeTurnHandler,
        *,
        conversation_mode: str = "device",
        wake_word_enabled: bool = False,
        wake_word: str = "hey willow",
        sleep_timeout_seconds: float = 30.0,
        on_device_volume: DeviceVolumeHandler | None = None,
    ) -> None:
        super().__init__(
            registry,
            hermes=None,  # type: ignore[arg-type]
            speech=speech,
            conversation_mode=conversation_mode,
            wake_word_enabled=wake_word_enabled,
            wake_word=wake_word,
            sleep_timeout_seconds=sleep_timeout_seconds,
            on_device_volume=on_device_volume,
        )
        self._handle_native_turn = handle_turn

    async def _run_turn(self, session: DeviceSession, turn: SpeechTurn, hello_metadata: dict[str, Any] | None) -> None:
        turn_started_at = time.monotonic()
        first_text_at: float | None = None
        completed_at: float | None = None
        latency: dict[str, Any] = {
            "hermes_submit_at": turn_started_at,
            "source": turn.source,
            "chars": len(turn.text),
        }
        for key in ("flux_start_at", "flux_first_transcript_at", "flux_end_at"):
            if key in turn.metadata:
                latency[key] = turn.metadata[key]
        session.device_state["_latency_turn"] = latency
        try:
            session.capturing_audio = False
            session.device_state["last_assistant_text"] = ""
            await self._set_assistant_state(session, "thinking")
            await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "thinking"}))
            logger.info(
                "Submitting native Hermes turn for %s conversation=%s source=%s chars=%s",
                session.device_id,
                session.conversation,
                turn.source,
                len(turn.text),
            )
            response_text = sanitize_spoken_text(await self._handle_native_turn(session, turn, hello_metadata) or "")
            if response_text:
                if self._media_mode_active(session):
                    logger.info(
                        "Suppressing native assistant speech while media is playing for %s: chars=%s",
                        session.device_id,
                        len(response_text),
                    )
                    await self._send(
                        session,
                        DataChannelMessage(type="assistant.text.delta", payload={"text": response_text}),
                    )
                    response_text = ""
            if response_text:
                first_text_at = time.monotonic()
                latency["hermes_first_text_at"] = first_text_at
                logger.info(
                    "LATENCY %s native_hermes_response after_submit=%s after_flux_eot=%s",
                    session.device_id,
                    _format_ms(first_text_at - turn_started_at),
                    _format_ms(first_text_at - latency["flux_end_at"])
                    if isinstance(latency.get("flux_end_at"), (int, float))
                    else "unknown",
                )
                await self._send(
                    session,
                    DataChannelMessage(type="assistant.text.delta", payload={"text": response_text}),
                )
                session.device_state["last_assistant_text"] = response_text[-2000:]
                await self._set_assistant_state(session, "speaking")
                await self._send(session, DataChannelMessage(type="assistant.state", payload={"state": "speaking"}))
                await self._speech.on_text_delta(session, response_text)

            completed_at = time.monotonic()
            latency["hermes_completed_at"] = completed_at
            logger.info(
                "LATENCY %s native_hermes_completed after_submit=%s after_first_text=%s",
                session.device_id,
                _format_ms(completed_at - turn_started_at),
                _format_ms(completed_at - first_text_at) if first_text_at is not None else "unknown",
            )
            await self._speech.on_turn_complete(session)
            await self._wait_for_output_playout(session, latency)
            self._log_latency_summary(session, latency)
            session.device_state["last_assistant_speech_ended_at"] = time.monotonic()
            await self._resume_listening(session)
        except asyncio.CancelledError:
            logger.info("Cancelled active native turn for %s", session.device_id)
            raise
        except Exception as exc:
            logger.exception("Native Hermes turn failed")
            await self._send(session, DataChannelMessage(type="error", payload={"message": str(exc)}))
            await self._resume_listening(session)
        finally:
            if session.connected:
                if self._media_mode_active(session):
                    session.capturing_audio = False
                    await self._set_assistant_state(session, "idle")
                else:
                    session.capturing_audio = True
                    await self._set_assistant_state(session, "listening")
            else:
                await self._set_assistant_state(session, "idle")
