from __future__ import annotations

import asyncio
import contextlib
import logging
import shutil
import time
from collections import deque
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .config import Settings
from .models import DataChannelMessage
from .registry import DeviceSession, InMemoryRegistry

if TYPE_CHECKING:
    from .speech import SpeechRuntime

logger = logging.getLogger(__name__)


class LibrespotService:
    def __init__(self, settings: Settings, registry: InMemoryRegistry, speech: "SpeechRuntime | None" = None) -> None:
        self._settings = settings
        self._registry = registry
        self._speech = speech
        self._process: asyncio.subprocess.Process | None = None
        self._ffmpeg_process: asyncio.subprocess.Process | None = None
        self._started_at: float | None = None
        self._log_lines: deque[str] = deque(maxlen=80)
        self._reader_tasks: list[asyncio.Task[None]] = []
        self._audio_tasks: list[asyncio.Task[None]] = []
        self._lock = asyncio.Lock()
        self._target_device_id: str | None = None
        self._audio_bytes_sent = 0
        self._audio_chunks_sent = 0

    async def start(self) -> dict[str, Any]:
        async with self._lock:
            self._reap_finished_process()
            if self._process is not None:
                return self.status()

            executable = shutil.which(self._settings.librespot_command)
            if executable is None:
                raise RuntimeError(f"librespot command not found: {self._settings.librespot_command}")

            target_session = await self._resolve_target_session()
            if self._settings.librespot_backend == "pipe":
                ffmpeg = shutil.which("ffmpeg")
                if ffmpeg is None:
                    raise RuntimeError("ffmpeg is required for librespot pipe playback")

            cache_dir = Path(self._settings.librespot_cache_dir).expanduser()
            cache_dir.mkdir(parents=True, exist_ok=True)

            command = [
                executable,
                "--name",
                self._settings.librespot_name,
                "--backend",
                self._settings.librespot_backend,
                "--cache",
                str(cache_dir),
                "--system-cache",
                str(cache_dir),
                "--initial-volume",
                str(self._settings.librespot_initial_volume),
                "--bitrate",
                str(self._settings.librespot_bitrate),
            ]
            if self._settings.librespot_device:
                command.extend(["--device", self._settings.librespot_device])

            self._log_lines.clear()
            self._target_device_id = target_session.device_id if target_session else None
            self._audio_bytes_sent = 0
            self._audio_chunks_sent = 0
            self._process = await asyncio.create_subprocess_exec(
                *command,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            self._started_at = time.time()
            if target_session is not None:
                await self._set_media_mode(target_session, playing=True)
            self._reader_tasks = [asyncio.create_task(self._read_stream("stderr", self._process.stderr))]
            if self._settings.librespot_backend == "pipe":
                self._ffmpeg_process = await self._start_pcm_converter()
                self._audio_tasks = [
                    asyncio.create_task(self._pump_librespot_to_ffmpeg()),
                    asyncio.create_task(self._pump_pcm_to_device(target_session)),
                    asyncio.create_task(self._read_stream("ffmpeg", self._ffmpeg_process.stderr)),
                ]
            else:
                self._reader_tasks.append(asyncio.create_task(self._read_stream("stdout", self._process.stdout)))
            return self.status()

    async def stop(self) -> dict[str, Any]:
        async with self._lock:
            process = self._process
            if process is None:
                return self.status()

            process.terminate()
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(process.wait(), timeout=5)
            if process.returncode is None:
                process.kill()
                await process.wait()

            await self._cancel_readers()
            await self._stop_ffmpeg()
            if self._target_device_id:
                session = await self._registry.get_session_for_device(self._target_device_id)
                if session is not None:
                    await self._set_media_mode(session, playing=False)
            self._process = None
            self._started_at = None
            self._target_device_id = None
            return self.status()

    async def close(self) -> None:
        await self.stop()

    def status(self) -> dict[str, Any]:
        self._reap_finished_process()
        running = self._process is not None
        return {
            "autostart_enabled": self._settings.librespot_enabled,
            "running": running,
            "pid": self._process.pid if self._process else None,
            "returncode": self._process.returncode if self._process else None,
            "started_at": self._started_at,
            "device_name": self._settings.librespot_name,
            "backend": self._settings.librespot_backend,
            "audio_device": self._settings.librespot_device,
            "target_device_id": self._target_device_id,
            "cache_dir": str(Path(self._settings.librespot_cache_dir).expanduser()),
            "audio_bytes_sent": self._audio_bytes_sent,
            "audio_chunks_sent": self._audio_chunks_sent,
            "logs": list(self._log_lines),
        }

    async def _resolve_target_session(self) -> DeviceSession | None:
        if self._settings.librespot_backend != "pipe":
            return None

        if self._settings.librespot_target_device_id:
            session = await self._registry.get_session_for_device(self._settings.librespot_target_device_id)
            if session and session.connected and session.output_track is not None:
                return session
            raise RuntimeError(
                f"target device is not connected with an output track: {self._settings.librespot_target_device_id}"
            )

        candidates = [
            session
            for _, session in await self._registry.list_devices()
            if session and session.connected and session.output_track is not None
        ]
        if not candidates:
            raise RuntimeError("no connected device output track for librespot pipe playback")
        if len(candidates) > 1:
            raise RuntimeError("multiple connected output devices; set HERMES_IOT_LIBRESPOT_TARGET_DEVICE_ID")
        return candidates[0]

    async def _start_pcm_converter(self) -> asyncio.subprocess.Process:
        ffmpeg = shutil.which("ffmpeg")
        if ffmpeg is None:
            raise RuntimeError("ffmpeg is required for librespot pipe playback")
        return await asyncio.create_subprocess_exec(
            ffmpeg,
            "-nostdin",
            "-v",
            "warning",
            "-f",
            "s16le",
            "-ar",
            "44100",
            "-ac",
            "2",
            "-i",
            "pipe:0",
            "-vn",
            "-ac",
            "1",
            "-af",
            "aresample=filter_size=64:phase_shift=10:dither_method=triangular",
            "-ar",
            str(self._settings.librespot_output_sample_rate),
            "-sample_fmt",
            "s16",
            "-f",
            "s16le",
            "pipe:1",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )

    async def _pump_librespot_to_ffmpeg(self) -> None:
        if self._process is None or self._process.stdout is None or self._ffmpeg_process is None:
            return
        ffmpeg_stdin = self._ffmpeg_process.stdin
        if ffmpeg_stdin is None:
            return
        try:
            while chunk := await self._process.stdout.read(4096):
                ffmpeg_stdin.write(chunk)
                await ffmpeg_stdin.drain()
        except (BrokenPipeError, ConnectionResetError):
            logger.info("librespot ffmpeg pipe closed")
        finally:
            with contextlib.suppress(Exception):
                ffmpeg_stdin.close()

    async def _pump_pcm_to_device(self, session: DeviceSession | None) -> None:
        if session is None or self._ffmpeg_process is None or self._ffmpeg_process.stdout is None:
            return
        sample_rate = self._settings.librespot_output_sample_rate
        chunk_samples = max(1, sample_rate // 50)
        chunk_bytes = chunk_samples * 2
        started_at = time.monotonic()
        samples_sent = 0
        try:
            while chunk := await self._ffmpeg_process.stdout.read(chunk_bytes):
                current = await self._registry.get_session_for_device(session.device_id)
                if current is None or not current.connected or current.output_track is None:
                    self._log_lines.append(f"audio: target disconnected: {session.device_id}")
                    if self._process and self._process.returncode is None:
                        self._process.terminate()
                    return
                if not current.device_state.get("media_playing"):
                    await self._set_media_mode(current, playing=True)
                samples_sent += len(chunk) // 2
                target_time = started_at + (samples_sent / sample_rate)
                delay = target_time - time.monotonic()
                if self._media_output_suppressed(current):
                    current.device_state["media_source"] = "spotify"
                    current.device_state["last_media_audio_at"] = time.time()
                    await self._registry.upsert_session(current)
                    if delay > 0:
                        await asyncio.sleep(delay)
                    elif delay < -1.0:
                        started_at = time.monotonic() - (samples_sent / sample_rate)
                    continue

                await current.output_track.push_pcm(chunk)
                if delay > 0:
                    await asyncio.sleep(delay)
                elif delay < -1.0:
                    started_at = time.monotonic() - (samples_sent / sample_rate)
                current.device_state["media_source"] = "spotify"
                current.device_state["last_media_audio_at"] = time.time()
                self._audio_bytes_sent += len(chunk)
                self._audio_chunks_sent += 1
        finally:
            current = await self._registry.get_session_for_device(session.device_id)
            if current is not None:
                await self._set_media_mode(current, playing=False)

    @staticmethod
    def _media_output_suppressed(session: DeviceSession) -> bool:
        if not session.device_state.get("media_playing"):
            return False
        if not session.device_state.get("wake_detected"):
            return False
        return session.assistant_state in {"listening", "thinking", "speaking", "tool"}

    async def _set_media_mode(self, session: DeviceSession, *, playing: bool) -> None:
        current = await self._registry.get_session_for_device(session.device_id)
        if current is None:
            current = session
        current.device_state["media_playing"] = playing
        if playing:
            current.capturing_audio = False
            current.device_state.pop("wake_detected", None)
            current.device_state.pop("media_barge_in", None)
            current.device_state.pop("media_capture_reenabled_at", None)
            current.device_state["media_source"] = "spotify"
            current.device_state["last_media_audio_at"] = time.time()
            current.device_state["media_capture_disabled_at"] = time.monotonic()
            if self._speech is not None:
                await self._speech.pause_audio_ingest(current)
        elif self._speech is not None and current.connected:
            current.capturing_audio = True
            await self._speech.resume_audio_ingest(current)
        await self._registry.upsert_session(current)
        if current.sender is None:
            return
        await current.sender(
            DataChannelMessage(
                type="device.command",
                payload={"type": "media.mode", "playing": playing},
            ).model_dump(mode="json")
        )

    async def _read_stream(self, label: str, stream: asyncio.StreamReader | None) -> None:
        if stream is None:
            return
        while line := await stream.readline():
            self._log_lines.append(f"{label}: {line.decode(errors='replace').rstrip()}")

    async def _cancel_readers(self) -> None:
        tasks = self._reader_tasks + self._audio_tasks
        self._reader_tasks = []
        self._audio_tasks = []
        for task in tasks:
            task.cancel()
        for task in tasks:
            with contextlib.suppress(asyncio.CancelledError):
                await task

    def _reap_finished_process(self) -> None:
        if self._process is not None and self._process.returncode is not None:
            self._process = None
            self._started_at = None

    async def _stop_ffmpeg(self) -> None:
        process = self._ffmpeg_process
        self._ffmpeg_process = None
        if process is None:
            return
        if process.returncode is None:
            process.terminate()
            with contextlib.suppress(asyncio.TimeoutError):
                await asyncio.wait_for(process.wait(), timeout=2)
            if process.returncode is None:
                process.kill()
                await process.wait()
