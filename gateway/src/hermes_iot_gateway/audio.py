from __future__ import annotations

import asyncio
from fractions import Fraction
import time

from aiortc.mediastreams import AudioStreamTrack
from av import AudioFrame
import numpy as np


class PCMQueueAudioTrack(AudioStreamTrack):
    kind = "audio"

    def __init__(self, *, sample_rate: int = 16000, channels: int = 1, max_queue_chunks: int = 500) -> None:
        super().__init__()
        if channels != 1:
            raise ValueError("PCMQueueAudioTrack currently supports mono audio only")
        self.sample_rate = sample_rate
        self.channels = channels
        self._queue: asyncio.Queue[bytes] = asyncio.Queue(maxsize=max(1, max_queue_chunks))
        self._buffer = bytearray()
        self._chunks: asyncio.Queue[bytes] = asyncio.Queue()
        self._pts = 0
        self._samples_per_frame = sample_rate * 10 // 1000
        self._bytes_per_frame = self._samples_per_frame * self.channels * 2
        self._started_at = time.monotonic()
        self._playout_audio_end_at = self._started_at

    async def recv(self) -> AudioFrame:
        await self._pace()
        await self._fill_chunks()

        try:
            payload = self._chunks.get_nowait()
        except asyncio.QueueEmpty:
            payload = bytes(self._bytes_per_frame)

        samples = np.frombuffer(payload, dtype=np.int16)
        frame = AudioFrame.from_ndarray(samples[None, :], layout="mono")
        frame.sample_rate = self.sample_rate
        frame.time_base = Fraction(1, self.sample_rate)
        frame.pts = self._pts
        self._pts += self._samples_per_frame
        return frame

    async def push_pcm(self, chunk: bytes) -> None:
        if chunk:
            duration_seconds = len(chunk) / (self.sample_rate * self.channels * 2)
            now = time.monotonic()
            self._playout_audio_end_at = max(self._playout_audio_end_at, now) + duration_seconds
            await self._queue.put(chunk)

    async def clear(self) -> None:
        self._buffer.clear()
        self._playout_audio_end_at = time.monotonic()
        while True:
            try:
                self._queue.get_nowait()
            except asyncio.QueueEmpty:
                break
        while True:
            try:
                self._chunks.get_nowait()
            except asyncio.QueueEmpty:
                break

    async def wait_for_playout_drain(
        self,
        *,
        padding_seconds: float = 1.5,
        max_wait_seconds: float = 90.0,
    ) -> None:
        deadline = time.monotonic() + max_wait_seconds
        while True:
            now = time.monotonic()
            target = self._playout_audio_end_at + padding_seconds
            if now >= target or now >= deadline:
                return
            await asyncio.sleep(min(0.05, target - now, deadline - now))

    async def _pace(self) -> None:
        target_time = self._started_at + (self._pts / self.sample_rate)
        delay = target_time - time.monotonic()
        if delay > 0:
            await asyncio.sleep(delay)
        elif delay < -1.0:
            self._started_at = time.monotonic() - (self._pts / self.sample_rate)

    async def _fill_chunks(self) -> None:
        while self._chunks.empty():
            try:
                chunk = await asyncio.wait_for(self._queue.get(), timeout=0.005)
            except TimeoutError:
                return
            self._buffer.extend(chunk)
            ready_bytes = (len(self._buffer) // self._bytes_per_frame) * self._bytes_per_frame
            for offset in range(0, ready_bytes, self._bytes_per_frame):
                self._chunks.put_nowait(bytes(self._buffer[offset : offset + self._bytes_per_frame]))
            del self._buffer[:ready_bytes]
