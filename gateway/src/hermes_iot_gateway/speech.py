from __future__ import annotations

import asyncio
import contextlib
import inspect
import logging
from dataclasses import dataclass
from typing import Awaitable, Callable, Protocol

from aiortc import MediaStreamTrack

from .models import DataChannelMessage, SpeechTurn
from .registry import DeviceSession

logger = logging.getLogger(__name__)


@dataclass(slots=True)
class _AudioIngestBinding:
    session: DeviceSession
    track: MediaStreamTrack
    emit: Callable
    on_turn: Callable
    on_interrupt: Callable


class SpeechToTextProvider(Protocol):
    async def transcribe_text_payload(self, text: str) -> SpeechTurn | None: ...

    async def start_audio_ingest(
        self,
        *,
        session: DeviceSession,
        track: MediaStreamTrack,
        emit,
        on_turn,
        on_interrupt,
    ) -> asyncio.Task[None] | None: ...


class TextToSpeechProvider(Protocol):
    async def attach_output(self, session: DeviceSession, push_audio: Callable[[bytes], Awaitable[None]]) -> None: ...

    async def detach_output(self, session: DeviceSession) -> None: ...

    async def on_text_delta(self, session: DeviceSession, text: str) -> None: ...

    async def on_turn_complete(self, session: DeviceSession) -> None: ...

    async def interrupt_output(self, session: DeviceSession) -> None: ...


class DebugSpeechToTextProvider:
    async def transcribe_text_payload(self, text: str) -> SpeechTurn | None:
        cleaned = text.strip()
        if not cleaned:
            return None
        return SpeechTurn(text=cleaned, source="debug_text")

    async def start_audio_ingest(
        self,
        *,
        session: DeviceSession,
        track: MediaStreamTrack,
        emit,
        on_turn,
        on_interrupt,
    ) -> asyncio.Task[None]:
        return asyncio.create_task(self._consume_audio(session=session, track=track, emit=emit))

    async def _consume_audio(self, *, session: DeviceSession, track: MediaStreamTrack, emit) -> None:
        frame_count = 0
        try:
            while True:
                await track.recv()
                frame_count += 1
                if session.capturing_audio and frame_count % 50 == 0:
                    await emit(
                        session,
                        DataChannelMessage(
                            type="audio.input.level",
                            payload={"frames_seen": frame_count, "state": session.assistant_state},
                        ),
                    )
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            logger.debug("Audio ingest ended for %s: %s", session.device_id, exc)


class NoOpTextToSpeechProvider:
    async def attach_output(self, session: DeviceSession, push_audio) -> None:
        return None

    async def detach_output(self, session: DeviceSession) -> None:
        return None

    async def on_text_delta(self, session: DeviceSession, text: str) -> None:
        return None

    async def on_turn_complete(self, session: DeviceSession) -> None:
        return None

    async def interrupt_output(self, session: DeviceSession) -> None:
        return None


class SpeechRuntime:
    def __init__(self, stt: SpeechToTextProvider, tts: TextToSpeechProvider) -> None:
        self._stt = stt
        self._tts = tts
        self._audio_tasks: dict[str, asyncio.Task[None]] = {}
        self._audio_bindings: dict[str, _AudioIngestBinding] = {}

    async def transcribe_text_payload(self, text: str) -> SpeechTurn | None:
        return await self._stt.transcribe_text_payload(text)

    async def attach_audio_track(self, *, session: DeviceSession, track: MediaStreamTrack, emit, on_turn, on_interrupt) -> None:
        await self.pause_audio_ingest(session)
        self._audio_bindings[session.session_id] = _AudioIngestBinding(
            session=session,
            track=track,
            emit=emit,
            on_turn=on_turn,
            on_interrupt=on_interrupt,
        )
        if session.capturing_audio:
            await self.resume_audio_ingest(session)

    async def pause_audio_ingest(self, session: DeviceSession) -> None:
        task = self._audio_tasks.pop(session.session_id, None)
        if task:
            logger.info("Closing STT audio ingest for %s", session.device_id)
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task

    async def resume_audio_ingest(self, session: DeviceSession) -> None:
        if session.session_id in self._audio_tasks:
            return
        binding = self._audio_bindings.get(session.session_id)
        if binding is None:
            return
        logger.info("Starting STT audio ingest for %s", session.device_id)
        task = await self._stt.start_audio_ingest(
            session=binding.session,
            track=binding.track,
            emit=binding.emit,
            on_turn=binding.on_turn,
            on_interrupt=binding.on_interrupt,
        )
        if task is not None:
            self._audio_tasks[session.session_id] = task

    async def attach_output_track(self, *, session: DeviceSession, push_audio) -> None:
        await self._tts.attach_output(session, push_audio)

    async def on_text_delta(self, session: DeviceSession, text: str) -> None:
        await self._tts.on_text_delta(session, text)

    async def on_turn_complete(self, session: DeviceSession) -> None:
        await self._tts.on_turn_complete(session)

    async def interrupt_output(self, session: DeviceSession) -> None:
        await self._tts.interrupt_output(session)
        output_track = session.output_track
        clear_output = getattr(output_track, "clear", None)
        if not callable(clear_output):
            return
        result = clear_output()
        if inspect.isawaitable(result):
            await result

    async def detach_session(self, session: DeviceSession) -> None:
        await self.pause_audio_ingest(session)
        self._audio_bindings.pop(session.session_id, None)
        await self._tts.detach_output(session)
