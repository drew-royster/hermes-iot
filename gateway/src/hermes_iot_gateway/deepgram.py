from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import audioop
import time
from dataclasses import dataclass
from typing import Any, Awaitable, Callable

from aiortc import MediaStreamTrack
from av import AudioResampler
from websockets.asyncio.client import connect

from .models import DataChannelMessage, SpeechTurn
from .registry import DeviceSession

logger = logging.getLogger(__name__)


def _format_ms(seconds: float) -> str:
    return f"{seconds * 1000:.0f}ms"


TurnCallback = Callable[[DeviceSession, SpeechTurn], Awaitable[None]]
InterruptCallback = Callable[[DeviceSession, str], Awaitable[None]]
EmitCallback = Callable[[DeviceSession, DataChannelMessage], Awaitable[None]]


@dataclass(slots=True)
class DeepgramFluxConfig:
    api_key: str
    model: str = "flux-general-en"
    sample_rate: int = 16000
    encoding: str = "linear16"
    eot_threshold: float = 0.7
    eager_eot_threshold: float | None = 0.7
    eot_timeout_ms: int = 1800


@dataclass(slots=True)
class DeepgramAuraConfig:
    api_key: str
    model: str = "aura-2-thalia-en"
    sample_rate: int = 48000
    encoding: str = "linear16"
    container: str = "none"


class DeepgramFluxSpeechToTextProvider:
    def __init__(self, config: DeepgramFluxConfig) -> None:
        self._config = config

    async def transcribe_text_payload(self, text: str) -> SpeechTurn | None:
        cleaned = text.strip()
        if not cleaned:
            return None
        return SpeechTurn(text=cleaned, source="debug_text", metadata={"mode": "debug-inject"})

    async def start_audio_ingest(
        self,
        *,
        session: DeviceSession,
        track: MediaStreamTrack,
        emit: EmitCallback,
        on_turn: TurnCallback,
        on_interrupt: InterruptCallback,
    ) -> asyncio.Task[None]:
        return asyncio.create_task(
            self._run_flux_session(
                session=session,
                track=track,
                emit=emit,
                on_turn=on_turn,
                on_interrupt=on_interrupt,
            )
        )

    async def _run_flux_session(
        self,
        *,
        session: DeviceSession,
        track: MediaStreamTrack,
        emit: EmitCallback,
        on_turn: TurnCallback,
        on_interrupt: InterruptCallback,
    ) -> None:
        url = (
            "wss://api.deepgram.com/v2/listen"
            f"?model={self._config.model}"
            f"&encoding={self._config.encoding}"
            f"&sample_rate={self._config.sample_rate}"
            f"&eot_threshold={self._config.eot_threshold}"
            f"&eot_timeout_ms={self._config.eot_timeout_ms}"
        )
        if self._config.eager_eot_threshold is not None:
            url += f"&eager_eot_threshold={self._config.eager_eot_threshold}"

        headers = {"Authorization": f"Token {self._config.api_key}"}
        resampler = AudioResampler(format="s16", layout="mono", rate=self._config.sample_rate)

        async with connect(url, additional_headers=headers, max_size=None) as websocket:
            logger.info("Connected Flux for %s", session.device_id)

            async def send_audio() -> None:
                frame_count = 0
                pcm_peak = 0
                pcm_rms_sum = 0
                pcm_rms_count = 0
                while True:
                    frame = await track.recv()
                    frame_count += 1
                    if frame_count % 500 == 0:
                        logger.info(
                            "Flux ingest frames for %s: count=%s rate=%s samples=%s",
                            session.device_id,
                            frame_count,
                            getattr(frame, "sample_rate", "unknown"),
                            getattr(frame, "samples", "unknown"),
                        )
                    resampled_frames = resampler.resample(frame)
                    if not isinstance(resampled_frames, list):
                        resampled_frames = [resampled_frames]
                    if not session.capturing_audio:
                        continue
                    for pcm_frame in resampled_frames:
                        if pcm_frame is None:
                            continue
                        pcm = bytes(pcm_frame.planes[0])
                        if pcm:
                            pcm_peak = max(pcm_peak, audioop.max(pcm, 2))
                            pcm_rms_sum += audioop.rms(pcm, 2)
                            pcm_rms_count += 1
                        if frame_count % 50 == 0:
                            avg_rms = (
                                pcm_rms_sum // pcm_rms_count
                                if pcm_rms_count
                                else 0
                            )
                            logger.info(
                                "Flux PCM level for %s: frames=%s peak=%s rms=%s capturing=%s state=%s",
                                session.device_id,
                                frame_count,
                                pcm_peak,
                                avg_rms,
                                session.capturing_audio,
                                session.assistant_state,
                            )
                            await emit(
                                session,
                                DataChannelMessage(
                                    type="audio.input.level",
                                    payload={
                                        "frames_seen": frame_count,
                                        "pcm_peak": pcm_peak,
                                        "pcm_rms": avg_rms,
                                        "capturing_audio": session.capturing_audio,
                                        "state": session.assistant_state,
                                    },
                                ),
                            )
                            pcm_peak = 0
                            pcm_rms_sum = 0
                            pcm_rms_count = 0
                        await websocket.send(pcm)

            async def receive_events() -> None:
                while True:
                    message = await websocket.recv()
                    if isinstance(message, bytes):
                        continue
                    payload = json.loads(message)
                    event = payload.get("event") or payload.get("type")
                    transcript = payload.get("transcript", "").strip()
                    now = time.monotonic()

                    if event in {"StartOfTurn", "TurnResumed"}:
                        session.device_state["_latency_turn"] = {
                            "flux_start_at": now,
                            "turn_index": payload.get("turn_index"),
                        }
                        logger.info(
                            "LATENCY %s flux_%s turn_index=%s",
                            session.device_id,
                            str(event).lower(),
                            payload.get("turn_index"),
                        )
                    if event == "Update" and session.capturing_audio:
                        latency = session.device_state.setdefault("_latency_turn", {})
                        if transcript and "flux_first_transcript_at" not in latency:
                            latency["flux_first_transcript_at"] = now
                            started_at = latency.get("flux_start_at")
                            if isinstance(started_at, (int, float)):
                                logger.info(
                                    "LATENCY %s flux_first_transcript=%.0fms",
                                    session.device_id,
                                    (now - started_at) * 1000,
                                )
                        await emit(
                            session,
                            DataChannelMessage(
                                type="audio.input.level",
                                payload={
                                    "event": event,
                                    "turn_index": payload.get("turn_index"),
                                    "has_transcript": bool(transcript),
                                    "transcript": transcript,
                                },
                            ),
                        )
                    if event == "EndOfTurn" and transcript:
                        if not session.capturing_audio:
                            logger.info(
                                "Ignoring Flux turn while capture is disabled for %s: %s",
                                session.device_id,
                                transcript,
                            )
                            continue
                        latency = session.device_state.setdefault("_latency_turn", {})
                        latency["flux_end_at"] = now
                        started_at = latency.get("flux_start_at")
                        first_transcript_at = latency.get("flux_first_transcript_at")
                        logger.info(
                            "LATENCY %s flux_end_of_turn start_to_eot=%s first_transcript_to_eot=%s chars=%s text=%s",
                            session.device_id,
                            _format_ms(now - started_at) if isinstance(started_at, (int, float)) else "unknown",
                            _format_ms(now - first_transcript_at)
                            if isinstance(first_transcript_at, (int, float))
                            else "unknown",
                            len(transcript),
                            transcript,
                        )
                        await on_turn(
                            session,
                            SpeechTurn(
                                text=transcript,
                                source="stt",
                                metadata={
                                    "provider": "deepgram-flux",
                                    "event": event,
                                    "turn_index": payload.get("turn_index"),
                                    "flux_start_at": started_at,
                                    "flux_first_transcript_at": first_transcript_at,
                                    "flux_end_at": now,
                                },
                            ),
                        )
                        await emit(
                            session,
                            DataChannelMessage(
                                type="audio.input.level",
                                payload={
                                    "event": event,
                                    "turn_index": payload.get("turn_index"),
                                    "has_transcript": True,
                                    "transcript": transcript,
                                    "final": True,
                                },
                            ),
                        )

            sender = asyncio.create_task(send_audio())
            receiver = asyncio.create_task(receive_events())
            try:
                await asyncio.gather(sender, receiver)
            finally:
                logger.info("Flux session closed for %s", session.device_id)
                sender.cancel()
                receiver.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await sender
                with contextlib.suppress(asyncio.CancelledError):
                    await receiver


class DeepgramAuraTextToSpeechProvider:
    def __init__(self, config: DeepgramAuraConfig) -> None:
        self._config = config
        self._connections: dict[str, Any] = {}
        self._receivers: dict[str, asyncio.Task[None]] = {}
        self._received_chunks: dict[str, int] = {}
        self._received_bytes: dict[str, int] = {}
        self._last_audio_at: dict[str, float] = {}

    async def attach_output(self, session: DeviceSession, push_audio: Callable[[bytes], Awaitable[None]]) -> None:
        if session.session_id in self._connections:
            await self.detach_output(session)
        url = (
            "wss://api.deepgram.com/v1/speak"
            f"?model={self._config.model}"
            f"&encoding={self._config.encoding}"
            f"&sample_rate={self._config.sample_rate}"
            f"&container={self._config.container}"
        )
        headers = {"Authorization": f"Token {self._config.api_key}"}
        websocket = await connect(url, additional_headers=headers, max_size=None)
        self._connections[session.session_id] = websocket
        self._received_chunks[session.session_id] = 0
        self._received_bytes[session.session_id] = 0
        self._last_audio_at.pop(session.session_id, None)
        logger.info(
            "Connected Aura for %s: model=%s sample_rate=%s encoding=%s",
            session.session_id,
            self._config.model,
            self._config.sample_rate,
            self._config.encoding,
        )
        self._receivers[session.session_id] = asyncio.create_task(self._receive_audio(session, websocket, push_audio))

    async def detach_output(self, session: DeviceSession) -> None:
        receiver = self._receivers.pop(session.session_id, None)
        if receiver:
            receiver.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await receiver
        websocket = self._connections.pop(session.session_id, None)
        if websocket is not None:
            await websocket.close()
        self._received_chunks.pop(session.session_id, None)
        self._received_bytes.pop(session.session_id, None)
        self._last_audio_at.pop(session.session_id, None)

    async def on_text_delta(self, session: DeviceSession, text: str) -> None:
        websocket = self._connections.get(session.session_id)
        if websocket is None or not text:
            return
        now = time.monotonic()
        latency = session.device_state.setdefault("_latency_turn", {})
        if "aura_first_text_at" not in latency:
            latency["aura_first_text_at"] = now
            hermes_first_text_at = latency.get("hermes_first_text_at")
            if isinstance(hermes_first_text_at, (int, float)):
                logger.info(
                    "LATENCY %s aura_first_text_after_hermes=%s",
                    session.device_id,
                    _format_ms(now - hermes_first_text_at),
                )
        logger.info("Sending Aura text delta for %s: chars=%s", session.session_id, len(text))
        await websocket.send(json.dumps({"type": "Speak", "text": text}))

    async def on_turn_complete(self, session: DeviceSession) -> None:
        websocket = self._connections.get(session.session_id)
        if websocket is None:
            return
        logger.info("Flushing Aura for %s", session.session_id)
        chunks_before_flush = self._received_chunks.get(session.session_id, 0)
        await websocket.send(json.dumps({"type": "Flush"}))
        await self._wait_for_audio_drain(session.session_id, chunks_before_flush)
        latency = session.device_state.setdefault("_latency_turn", {})
        latency["aura_drain_at"] = time.monotonic()

    async def _wait_for_audio_drain(
        self,
        session_id: str,
        chunks_before_flush: int,
        *,
        silence_seconds: float = 0.25,
        first_audio_timeout_seconds: float = 1.5,
        max_wait_seconds: float = 90.0,
    ) -> None:
        started_at = time.monotonic()
        while True:
            now = time.monotonic()
            chunks = self._received_chunks.get(session_id, 0)
            last_audio_at = self._last_audio_at.get(session_id)
            received_flush_audio = chunks > chunks_before_flush

            if received_flush_audio and last_audio_at is not None and now - last_audio_at >= silence_seconds:
                logger.info("Aura drained for %s after %.2fs", session_id, now - started_at)
                return
            if not received_flush_audio and now - started_at >= first_audio_timeout_seconds:
                logger.info("Aura drain skipped for %s; no post-flush audio", session_id)
                return
            if now - started_at >= max_wait_seconds:
                logger.warning("Aura drain timed out for %s after %.2fs", session_id, now - started_at)
                return
            await asyncio.sleep(0.05)

    async def _receive_audio(self, session: DeviceSession, websocket, push_audio: Callable[[bytes], Awaitable[None]]) -> None:
        while True:
            message = await websocket.recv()
            if isinstance(message, bytes):
                session_id = session.session_id
                now = time.monotonic()
                session.device_state["last_assistant_audio_at"] = now
                self._last_audio_at[session_id] = now
                latency = session.device_state.setdefault("_latency_turn", {})
                if "aura_first_audio_at" not in latency:
                    latency["aura_first_audio_at"] = now
                    aura_first_text_at = latency.get("aura_first_text_at")
                    hermes_first_text_at = latency.get("hermes_first_text_at")
                    logger.info(
                        "LATENCY %s aura_first_audio after_aura_text=%s after_hermes_first_text=%s",
                        session.device_id,
                        _format_ms(now - aura_first_text_at)
                        if isinstance(aura_first_text_at, (int, float))
                        else "unknown",
                        _format_ms(now - hermes_first_text_at)
                        if isinstance(hermes_first_text_at, (int, float))
                        else "unknown",
                    )
                chunk_count = self._received_chunks.get(session_id, 0) + 1
                total_bytes = self._received_bytes.get(session_id, 0) + len(message)
                self._received_chunks[session_id] = chunk_count
                self._received_bytes[session_id] = total_bytes
                if chunk_count == 1 or (chunk_count % 25) == 0:
                    audio_seconds = total_bytes / (self._config.sample_rate * 2)
                    logger.info(
                        "Aura audio for %s: chunks=%s last_bytes=%s total_bytes=%s estimated_seconds=%.2f",
                        session_id,
                        chunk_count,
                        len(message),
                        total_bytes,
                        audio_seconds,
                    )
                await push_audio(message)
                continue
            try:
                payload = json.loads(message)
            except json.JSONDecodeError:
                logger.debug("Ignoring non-json TTS control message: %s", message)
                continue
            if payload.get("type") == "Warning":
                logger.warning("Deepgram Aura warning for %s: %s", session_id, payload)
