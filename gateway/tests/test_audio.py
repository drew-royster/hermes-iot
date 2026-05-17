import asyncio

from hermes_iot_gateway.audio import PCMQueueAudioTrack
from hermes_iot_gateway.registry import DeviceSession
from hermes_iot_gateway.speech import NoOpTextToSpeechProvider, SpeechRuntime


async def _track_frames_fixed_10ms_chunks() -> None:
    track = PCMQueueAudioTrack(sample_rate=16000, channels=1)
    frame_bytes = 160 * 2
    await track.push_pcm((b"\x01\x00" * 160) + (b"\x02\x00" * 160))

    first = await track.recv()
    second = await track.recv()

    assert first.sample_rate == 16000
    assert first.samples == 160
    assert first.pts == 0
    assert bytes(first.planes[0]) == b"\x01\x00" * 160
    assert second.sample_rate == 16000
    assert second.samples == 160
    assert second.pts == 160
    assert bytes(second.planes[0]) == b"\x02\x00" * 160
    assert frame_bytes == len(bytes(first.planes[0]))


def test_pcm_queue_audio_track_frames_fixed_10ms_chunks() -> None:
    asyncio.run(_track_frames_fixed_10ms_chunks())


async def _track_backpressures_when_queue_is_full() -> None:
    track = PCMQueueAudioTrack(sample_rate=16000, channels=1, max_queue_chunks=1)
    await track.push_pcm(b"\x01\x00" * 160)
    second_push = asyncio.create_task(track.push_pcm(b"\x02\x00" * 160))

    await asyncio.sleep(0)
    assert not second_push.done()

    frame = await track.recv()
    await second_push
    second_frame = await track.recv()

    assert bytes(frame.planes[0]) == b"\x01\x00" * 160
    assert bytes(second_frame.planes[0]) == b"\x02\x00" * 160


def test_pcm_queue_audio_track_backpressures_when_queue_is_full() -> None:
    asyncio.run(_track_backpressures_when_queue_is_full())


async def _track_clear_drops_buffered_audio() -> None:
    track = PCMQueueAudioTrack(sample_rate=16000, channels=1)
    await track.push_pcm((b"\x01\x00" * 160) + (b"\x02\x00" * 160))

    first = await track.recv()
    await track.clear()
    second = await track.recv()

    assert bytes(first.planes[0]) == b"\x01\x00" * 160
    assert bytes(second.planes[0]) == b"\x00\x00" * 160


def test_pcm_queue_audio_track_clear_drops_buffered_audio() -> None:
    asyncio.run(_track_clear_drops_buffered_audio())


class _FakeSttProvider:
    def __init__(self) -> None:
        self.started = 0
        self.cancelled = 0

    async def transcribe_text_payload(self, text: str):
        return None

    async def start_audio_ingest(self, **kwargs):
        self.started += 1

        async def _run() -> None:
            try:
                while True:
                    await asyncio.sleep(60)
            except asyncio.CancelledError:
                self.cancelled += 1
                raise

        return asyncio.create_task(_run())


class _FakeTtsProvider(NoOpTextToSpeechProvider):
    def __init__(self) -> None:
        self.interrupted: list[str] = []

    async def interrupt_output(self, session: DeviceSession) -> None:
        self.interrupted.append(session.device_id)


async def _speech_runtime_can_pause_and_resume_audio_ingest() -> None:
    stt = _FakeSttProvider()
    runtime = SpeechRuntime(stt, NoOpTextToSpeechProvider())
    session = DeviceSession(
        session_id="session-1",
        device_id="device-1",
        conversation="iot:device-1",
        capturing_audio=True,
    )

    await runtime.attach_audio_track(
        session=session,
        track=object(),
        emit=None,
        on_turn=None,
        on_interrupt=None,
    )
    await asyncio.sleep(0)
    await runtime.pause_audio_ingest(session)
    await runtime.resume_audio_ingest(session)
    await asyncio.sleep(0)
    await runtime.detach_session(session)

    assert stt.started == 2
    assert stt.cancelled == 2


def test_speech_runtime_can_pause_and_resume_audio_ingest() -> None:
    asyncio.run(_speech_runtime_can_pause_and_resume_audio_ingest())


async def _speech_runtime_interrupt_output_clears_track() -> None:
    stt = _FakeSttProvider()
    tts = _FakeTtsProvider()
    runtime = SpeechRuntime(stt, tts)
    track = PCMQueueAudioTrack(sample_rate=16000, channels=1)
    await track.push_pcm(b"\x01\x00" * 160)
    session = DeviceSession(
        session_id="session-1",
        device_id="device-1",
        conversation="iot:device-1",
        output_track=track,
    )

    await runtime.interrupt_output(session)
    frame = await track.recv()

    assert tts.interrupted == ["device-1"]
    assert bytes(frame.planes[0]) == b"\x00\x00" * 160


def test_speech_runtime_interrupt_output_clears_track() -> None:
    asyncio.run(_speech_runtime_interrupt_output_clears_track())
