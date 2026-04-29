import asyncio
import contextlib

from hermes_iot_gateway.models import HermesStreamEvent
from hermes_iot_gateway.registry import InMemoryRegistry
from hermes_iot_gateway.session import GatewaySessionManager


class _FakeHermes:
    def __init__(self) -> None:
        self.calls: list[dict] = []

    async def stream_text_turn(self, **kwargs):
        self.calls.append(kwargs)
        yield HermesStreamEvent(kind="response.created", payload={"id": "resp_1"})
        yield HermesStreamEvent(
            kind="tool.call",
            payload={"name": "iot_set_led", "arguments": "{}", "call_id": "call_1"},
        )
        yield HermesStreamEvent(
            kind="tool.output",
            payload={"call_id": "call_1", "output": "{\"accepted\": true}"},
        )
        yield HermesStreamEvent(kind="assistant.text.delta", payload={"text": "Hello"})
        yield HermesStreamEvent(kind="response.completed", payload={"response": {"id": "resp_1"}})


class _FakeSpeech:
    def __init__(self) -> None:
        self.text_deltas: list[str] = []
        self.paused: list[str] = []
        self.resumed: list[str] = []
        self.attached_audio: list[str] = []
        self.detached: list[str] = []

    async def attach_audio_track(self, **kwargs):
        self.attached_audio.append(kwargs["session"].device_id)
        return None

    async def attach_output_track(self, **kwargs):
        return None

    async def detach_session(self, session):
        self.detached.append(session.device_id)
        return None

    async def pause_audio_ingest(self, session):
        self.paused.append(session.device_id)
        return None

    async def resume_audio_ingest(self, session):
        self.resumed.append(session.device_id)

    async def transcribe_text_payload(self, text: str):
        from hermes_iot_gateway.models import SpeechTurn

        return SpeechTurn(text=text, source="debug_text")

    async def on_text_delta(self, session, text: str):
        self.text_deltas.append(text)

    async def on_turn_complete(self, session):
        return None


class _BrokenHermes:
    async def stream_text_turn(self, **kwargs):
        raise RuntimeError("backend blew up")
        yield kwargs


class _SlowHermes:
    async def stream_text_turn(self, **kwargs):
        yield HermesStreamEvent(kind="response.created", payload={"id": "resp_slow"})
        await asyncio.sleep(0.2)
        yield HermesStreamEvent(kind="assistant.text.delta", payload={"text": "Too late"})
        yield HermesStreamEvent(kind="response.completed", payload={"response": {"id": "resp_slow"}})


class _RecordedSlowHermes:
    def __init__(self) -> None:
        self.calls: list[dict] = []

    async def stream_text_turn(self, **kwargs):
        self.calls.append(kwargs)
        yield HermesStreamEvent(kind="response.created", payload={"id": f"resp_{len(self.calls)}"})
        await asyncio.sleep(0.2)
        yield HermesStreamEvent(kind="assistant.text.delta", payload={"text": kwargs["text"]})
        yield HermesStreamEvent(kind="response.completed", payload={"response": {"id": f"resp_{len(self.calls)}"}})


class _FakePeer:
    def __init__(self) -> None:
        self.closed = False

    async def close(self) -> None:
        self.closed = True


async def _session_state_roundtrip() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-session",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    hermes = _FakeHermes()
    manager = GatewaySessionManager(registry, hermes, speech)
    session = await manager.create_session("echo-session")

    sent_messages: list[dict] = []

    async def _sender(payload: dict) -> None:
        sent_messages.append(payload)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "hello world", "hello": {"simulator": True}}},
    )
    assert session.active_turn is not None
    await session.active_turn

    state_messages = [msg for msg in sent_messages if msg["type"] == "assistant.state"]
    tool_messages = [msg for msg in sent_messages if msg["type"] == "tool.progress"]
    text_messages = [msg for msg in sent_messages if msg["type"] == "assistant.text.delta"]

    assert [msg["payload"]["state"] for msg in state_messages] == ["thinking", "tool", "speaking", "idle", "listening"]
    assert tool_messages[0]["payload"]["phase"] == "call"
    assert tool_messages[1]["payload"]["phase"] == "output"
    assert text_messages[0]["payload"]["text"] == "Hello"
    assert speech.text_deltas == ["Hello"]
    assert hermes.calls[0]["conversation"] == "iot:echo-session"


def test_session_manager_emits_expected_state_sequence() -> None:
    asyncio.run(_session_state_roundtrip())


async def _session_hello_without_media_starts_listening() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-hello-listen",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _FakeHermes(), speech)
    session = await manager.create_session("echo-hello-listen")

    sent_messages: list[dict] = []

    async def _sender(payload: dict) -> None:
        sent_messages.append(payload)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "hello", "payload": {"device_id": "echo-hello-listen", "capabilities": ["mic", "speaker"]}},
    )

    persisted = await registry.get_session(session.session_id)
    assert persisted is not None
    assert persisted.capturing_audio is True
    assert persisted.assistant_state == "listening"
    assert speech.paused == []
    assert speech.resumed == []
    assert sent_messages == [
        {
            "type": "assistant.state",
            "payload": {
                "state": "listening",
                "reason": "hello",
                "session_id": session.session_id,
                "conversation": session.conversation,
            },
            "timestamp": sent_messages[0]["timestamp"],
        }
    ]


def test_session_hello_without_media_starts_listening() -> None:
    asyncio.run(_session_hello_without_media_starts_listening())


async def _session_attach_audio_track_follows_current_mode() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-audio-track",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _FakeHermes(), speech)
    session = await manager.create_session("echo-audio-track")

    sent_messages: list[dict] = []

    async def _sender(payload: dict) -> None:
        sent_messages.append(payload)

    await manager.bind_sender(session.session_id, _sender)
    await manager.attach_audio_track(session.session_id, object())
    assert session.capturing_audio is True
    assert session.assistant_state == "listening"
    assert speech.attached_audio == ["echo-audio-track"]
    assert sent_messages[-1]["type"] == "assistant.state"
    assert sent_messages[-1]["payload"]["reason"] == "audio_attached"

    session.device_state["media_playing"] = True
    sent_messages.clear()
    await manager.attach_audio_track(session.session_id, object())
    assert session.capturing_audio is False
    assert session.assistant_state == "idle"
    assert speech.paused == ["echo-audio-track"]
    assert [msg["type"] for msg in sent_messages] == ["device.command", "assistant.state"]
    assert sent_messages[0]["payload"] == {"type": "media.mode", "playing": True}
    assert sent_messages[1]["payload"]["reason"] == "audio_attached"


def test_session_attach_audio_track_follows_current_mode() -> None:
    asyncio.run(_session_attach_audio_track_follows_current_mode())


async def _session_suppresses_assistant_speech_while_media_playing() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-media-speech",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _FakeHermes(), speech)
    session = await manager.create_session("echo-media-speech")
    session.device_state["media_playing"] = True

    sent_messages: list[dict] = []

    async def _sender(payload: dict) -> None:
        sent_messages.append(payload)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "play music"}},
    )
    assert session.active_turn is not None
    await session.active_turn

    state_messages = [msg for msg in sent_messages if msg["type"] == "assistant.state"]
    assert [msg["payload"]["state"] for msg in state_messages] == ["thinking", "tool", "idle"]
    assert state_messages[-1]["payload"]["media_playing"] is True
    assert speech.text_deltas == []
    assert session.capturing_audio is False


def test_session_suppresses_assistant_speech_while_media_playing() -> None:
    asyncio.run(_session_suppresses_assistant_speech_while_media_playing())


async def _session_scoped_conversation_roundtrip() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-session-mode",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    hermes = _FakeHermes()
    manager = GatewaySessionManager(registry, hermes, speech, conversation_mode="session")
    session = await manager.create_session("echo-session-mode")

    async def _sender(_: dict) -> None:
        return None

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "hello world"}},
    )
    assert session.active_turn is not None
    await session.active_turn

    assert session.conversation == f"iot:echo-session-mode:session:{session.session_id}"
    assert hermes.calls[0]["conversation"] == session.conversation


def test_session_manager_can_use_session_scoped_conversation() -> None:
    asyncio.run(_session_scoped_conversation_roundtrip())


async def _session_error_recovery_roundtrip() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-error",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _BrokenHermes(), speech)
    session = await manager.create_session("echo-error")

    sent_messages: list[dict] = []

    async def _sender(payload: dict) -> None:
        sent_messages.append(payload)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "hello world"}},
    )
    assert session.active_turn is not None
    await session.active_turn

    state_messages = [msg for msg in sent_messages if msg["type"] == "assistant.state"]
    error_messages = [msg for msg in sent_messages if msg["type"] == "error"]

    assert error_messages[0]["payload"]["message"] == "backend blew up"
    assert [msg["payload"]["state"] for msg in state_messages] == ["thinking", "idle", "listening"]
    assert session.assistant_state == "listening"


def test_session_manager_recovers_to_listening_after_backend_error() -> None:
    asyncio.run(_session_error_recovery_roundtrip())


async def _session_interrupt_roundtrip() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-interrupt",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _SlowHermes(), speech)
    session = await manager.create_session("echo-interrupt")

    sent_messages: list[dict] = []

    async def _sender(payload: dict) -> None:
        sent_messages.append(payload)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "hello world"}},
    )
    await asyncio.sleep(0.05)
    interrupted = await manager.interrupt("echo-interrupt", "device")
    assert interrupted is True
    with contextlib.suppress(asyncio.CancelledError):
        if session.active_turn is not None:
            await session.active_turn

    state_messages = [msg for msg in sent_messages if msg["type"] == "assistant.state"]
    text_messages = [msg for msg in sent_messages if msg["type"] == "assistant.text.delta"]

    assert [msg["payload"]["state"] for msg in state_messages] == ["thinking", "idle", "listening"]
    assert text_messages == []
    assert session.assistant_state == "listening"


def test_session_manager_resumes_listening_after_interrupt() -> None:
    asyncio.run(_session_interrupt_roundtrip())


async def _session_supersedes_thinking_turn_roundtrip() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-supersede-thinking",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    hermes = _RecordedSlowHermes()
    manager = GatewaySessionManager(registry, hermes, speech)
    session = await manager.create_session("echo-supersede-thinking")

    sent_messages: list[dict] = []

    async def _sender(payload: dict) -> None:
        sent_messages.append(payload)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "first turn"}},
    )
    await asyncio.sleep(0.05)
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "second turn"}},
    )
    assert session.active_turn is not None
    await session.active_turn

    text_messages = [msg for msg in sent_messages if msg["type"] == "assistant.text.delta"]
    assert [call["text"] for call in hermes.calls] == ["first turn", "second turn"]
    assert [msg["payload"]["text"] for msg in text_messages] == ["second turn"]
    assert speech.text_deltas == ["second turn"]
    assert session.assistant_state == "listening"


def test_session_manager_supersedes_thinking_turns_for_voice() -> None:
    asyncio.run(_session_supersedes_thinking_turn_roundtrip())


async def _wake_word_gating_roundtrip() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-wake",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    hermes = _FakeHermes()
    manager = GatewaySessionManager(
        registry,
        hermes,
        speech,
        wake_word_enabled=True,
        wake_word="hey willow",
        sleep_timeout_seconds=0.1,
    )
    session = await manager.create_session("echo-wake")

    sent_messages: list[dict] = []

    async def _sender(payload: dict) -> None:
        sent_messages.append(payload)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "hello", "payload": {"device_id": "echo-wake", "capabilities": ["mic", "speaker"]}},
    )
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "background kitchen noise"}},
    )
    assert hermes.calls == []
    assert session.assistant_state == "sleeping"

    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "hey willow what time is it"}},
    )
    assert session.active_turn is not None
    await session.active_turn

    assert hermes.calls[0]["text"] == "what time is it"
    assert speech.text_deltas == ["Hello"]
    assert session.assistant_state == "listening"

    await asyncio.sleep(0.15)
    assert session.assistant_state == "sleeping"
    state_messages = [msg for msg in sent_messages if msg["type"] == "assistant.state"]
    assert state_messages[0]["payload"]["state"] == "sleeping"
    assert any(msg["payload"]["reason"] == "wake_word" for msg in state_messages)
    assert state_messages[-1]["payload"]["reason"] == "inactivity_timeout"


def test_session_manager_gates_sleeping_transcripts_until_wake_word() -> None:
    asyncio.run(_wake_word_gating_roundtrip())


async def _session_device_state_roundtrip() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-state",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _FakeHermes(), speech)
    session = await manager.create_session("echo-state")

    async def _sender(_: dict) -> None:
        return None

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "device.state", "payload": {"muted": False, "battery": 92, "transport": "webrtc"}},
    )
    await manager.handle_control_message(
        session.session_id,
        {"type": "mute.set", "payload": {"muted": True}},
    )
    await manager.handle_control_message(
        session.session_id,
        {"type": "volume.set", "payload": {"volume": 42}},
    )
    await manager.handle_control_message(
        session.session_id,
        {
            "type": "audio.stats",
            "payload": {
                "mic_peak": 1200,
                "ref_peak": 48,
                "playback_underruns": 0,
                "playback_overflows": 0,
            },
        },
    )
    await manager.handle_control_message(
        session.session_id,
        {
            "type": "audio.aec_probe",
            "payload": {
                "result": "ESP_OK",
                "mic_rms": 1200.0,
                "ref_rms": 300.0,
                "aec_rms": 400.0,
                "suppression_db": 9.5,
            },
        },
    )

    persisted = await registry.get_session(session.session_id)
    assert persisted is not None
    assert persisted.device_state == {
        "muted": True,
        "battery": 92,
        "transport": "webrtc",
        "volume": 42,
        "audio_stats": {
            "mic_peak": 1200,
            "ref_peak": 48,
            "playback_underruns": 0,
            "playback_overflows": 0,
        },
        "audio_aec_probe": {
            "result": "ESP_OK",
            "mic_rms": 1200.0,
            "ref_rms": 300.0,
            "aec_rms": 400.0,
            "suppression_db": 9.5,
        },
    }


def test_session_manager_tracks_device_state_messages() -> None:
    asyncio.run(_session_device_state_roundtrip())


async def _session_volume_callback_roundtrip() -> None:
    registry = InMemoryRegistry()
    calls: list[tuple[str, int]] = []

    async def _on_volume(session, volume: int) -> None:
        calls.append((session.device_id, volume))

    manager = GatewaySessionManager(registry, _FakeHermes(), _FakeSpeech(), on_device_volume=_on_volume)
    session = await manager.create_session("echo-volume")

    await manager.handle_control_message(
        session.session_id,
        {"type": "volume.set", "payload": {"volume": 142}},
    )

    persisted = await registry.get_session(session.session_id)
    assert persisted is not None
    assert persisted.device_state["volume"] == 100
    assert calls == [("echo-volume", 100)]


def test_session_manager_calls_device_volume_handler() -> None:
    asyncio.run(_session_volume_callback_roundtrip())


async def _session_local_wake_reenables_capture_during_media() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-media",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    manager = GatewaySessionManager(registry, _FakeHermes(), _FakeSpeech())
    session = await manager.create_session("echo-media")
    session.capturing_audio = False
    session.device_state["media_playing"] = True

    sent_messages: list[dict] = []

    async def _sender(message: dict) -> None:
        sent_messages.append(message)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "device.state", "payload": {"wake_detected": True, "media_barge_in": True}},
    )

    persisted = await registry.get_session(session.session_id)
    assert persisted is not None
    assert persisted.capturing_audio is True
    assert persisted.assistant_state == "listening"
    assert manager._speech.resumed == ["echo-media"]
    assert sent_messages[-1]["type"] == "assistant.state"
    assert sent_messages[-1]["payload"]["reason"] == "local_wake"


def test_session_local_wake_reenables_capture_during_media() -> None:
    asyncio.run(_session_local_wake_reenables_capture_during_media())


async def _session_local_wake_hello_does_not_pause_media_capture() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-media-hello",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _FakeHermes(), speech)
    session = await manager.create_session("echo-media-hello")
    session.capturing_audio = False
    session.device_state["media_playing"] = True

    sent_messages: list[dict] = []

    async def _sender(message: dict) -> None:
        sent_messages.append(message)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {
            "type": "hello",
            "payload": {
                "device_id": "echo-media-hello",
                "capabilities": ["mic", "speaker"],
                "wake_detected": True,
                "media_barge_in": True,
            },
        },
    )

    persisted = await registry.get_session(session.session_id)
    assert persisted is not None
    assert persisted.capturing_audio is True
    assert persisted.assistant_state == "listening"
    assert speech.paused == []
    assert speech.resumed == ["echo-media-hello"]
    assert [msg["type"] for msg in sent_messages] == ["assistant.state"]
    assert sent_messages[0]["payload"]["reason"] == "local_wake"


def test_session_local_wake_hello_does_not_pause_media_capture() -> None:
    asyncio.run(_session_local_wake_hello_does_not_pause_media_capture())


async def _session_media_command_supersedes_slow_active_turn() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-media-supersede",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    hermes = _RecordedSlowHermes()
    manager = GatewaySessionManager(registry, hermes, _FakeSpeech())
    session = await manager.create_session("echo-media-supersede")
    session.device_state["media_playing"] = True

    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "play classical music"}},
    )
    assert session.active_turn is not None
    first_turn = session.active_turn
    await asyncio.sleep(0.05)

    await manager.handle_control_message(
        session.session_id,
        {"type": "device.state", "payload": {"wake_detected": True, "media_barge_in": True}},
    )
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "skip this song"}},
    )
    assert session.active_turn is not None
    assert session.active_turn is not first_turn
    with contextlib.suppress(asyncio.CancelledError):
        await first_turn
    await session.active_turn

    assert [call["text"] for call in hermes.calls] == ["play classical music", "skip this song"]


def test_session_media_command_supersedes_slow_active_turn() -> None:
    asyncio.run(_session_media_command_supersedes_slow_active_turn())


async def _session_media_mode_survives_sender_bind_and_hello() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-media-bind",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _FakeHermes(), speech)
    session = await manager.create_session("echo-media-bind")
    session.capturing_audio = True
    session.device_state["media_playing"] = True
    await registry.upsert_session(session)

    sent_messages: list[dict] = []

    async def _sender(message: dict) -> None:
        sent_messages.append(message)

    await manager.bind_sender(session.session_id, _sender)
    assert sent_messages == []
    assert speech.paused == []
    assert session.capturing_audio is True

    await manager.handle_control_message(
        session.session_id,
        {
            "type": "hello",
            "payload": {
                "device_id": "echo-media-bind",
                "capabilities": ["speaker", "mic"],
            },
        },
    )

    persisted = await registry.get_session(session.session_id)
    assert persisted is not None
    assert persisted.capturing_audio is False
    assert persisted.assistant_state == "idle"
    assert speech.paused == ["echo-media-bind"]
    assert [msg["type"] for msg in sent_messages].count("device.command") == 1
    assert not any(
        msg["type"] == "assistant.state" and msg["payload"]["state"] == "listening"
        for msg in sent_messages
    )


def test_session_media_mode_survives_sender_bind_and_hello() -> None:
    asyncio.run(_session_media_mode_survives_sender_bind_and_hello())


async def _session_media_barge_in_sequence_survives_bind_hello_and_device_state() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-media-barge-sequence",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _FakeHermes(), speech)
    session = await manager.create_session("echo-media-barge-sequence")
    session.capturing_audio = False
    session.device_state["media_playing"] = True
    await registry.upsert_session(session)

    sent_messages: list[dict] = []

    async def _sender(message: dict) -> None:
        sent_messages.append(message)

    await manager.bind_sender(session.session_id, _sender)
    assert sent_messages == []
    await manager.handle_control_message(
        session.session_id,
        {
            "type": "hello",
            "payload": {
                "device_id": "echo-media-barge-sequence",
                "capabilities": ["speaker", "mic"],
                "wake_detected": True,
                "media_barge_in": True,
            },
        },
    )
    await manager.handle_control_message(
        session.session_id,
        {"type": "device.state", "payload": {"wake_detected": True, "media_barge_in": True}},
    )

    persisted = await registry.get_session(session.session_id)
    assert persisted is not None
    assert persisted.capturing_audio is True
    assert persisted.assistant_state == "listening"
    assert speech.paused == []
    assert speech.resumed == ["echo-media-barge-sequence", "echo-media-barge-sequence"]
    assert not any(msg["type"] == "device.command" for msg in sent_messages)
    assert [msg["payload"]["reason"] for msg in sent_messages if msg["type"] == "assistant.state"] == [
        "local_wake",
        "local_wake",
    ]


def test_session_media_barge_in_sequence_survives_bind_hello_and_device_state() -> None:
    asyncio.run(_session_media_barge_in_sequence_survives_bind_hello_and_device_state())


async def _session_media_turn_stays_captured_until_turn_finishes() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-media-turn",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    hermes = _FakeHermes()
    manager = GatewaySessionManager(registry, hermes, speech)
    session = await manager.create_session("echo-media-turn")
    session.device_state["media_playing"] = True
    session.capturing_audio = False

    sent_messages: list[dict] = []

    async def _sender(message: dict) -> None:
        sent_messages.append(message)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {
            "type": "hello",
            "payload": {
                "device_id": "echo-media-turn",
                "capabilities": ["speaker", "mic"],
                "wake_detected": True,
                "media_barge_in": True,
            },
        },
    )
    assert session.capturing_audio is True

    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "skip the song"}},
    )
    assert session.active_turn is not None
    await session.active_turn

    assert [call["text"] for call in hermes.calls] == ["skip the song"]
    assert session.capturing_audio is False
    assert session.assistant_state == "idle"
    assert "wake_detected" not in session.device_state
    assert "media_barge_in" not in session.device_state
    assert "media_capture_reenabled_at" not in session.device_state
    assert speech.text_deltas == []
    assert sent_messages[-2]["type"] == "device.command"
    assert sent_messages[-2]["payload"] == {"type": "media.mode", "playing": True}
    assert sent_messages[-1]["type"] == "assistant.state"
    assert sent_messages[-1]["payload"]["reason"] == "media_playing"


def test_session_media_turn_stays_captured_until_turn_finishes() -> None:
    asyncio.run(_session_media_turn_stays_captured_until_turn_finishes())


async def _session_end_conversation_defers_until_turn_finishes() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-end",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    speech = _FakeSpeech()
    manager = GatewaySessionManager(registry, _SlowHermes(), speech)
    session = await manager.create_session("echo-end")
    peer = _FakePeer()
    session.peer = peer

    sent_messages: list[dict] = []

    async def _sender(message: dict) -> None:
        sent_messages.append(message)

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {"type": "debug.user_text", "payload": {"text": "that's all"}},
    )
    assert session.active_turn is not None
    await asyncio.sleep(0.05)

    result = await manager.end_conversation("echo-end")
    assert result == {"accepted": True, "status": "ending_after_turn", "device_id": "echo-end"}
    assert not any(msg["type"] == "device.command" for msg in sent_messages)
    assert session.capturing_audio is False
    assert speech.paused == ["echo-end"]

    await session.active_turn

    persisted = await registry.get_session(session.session_id)
    assert persisted is not None
    assert persisted.connected is False
    assert persisted.capturing_audio is False
    assert persisted.assistant_state == "idle"
    assert peer.closed is True
    assert speech.detached == ["echo-end"]
    assert sent_messages[-2]["type"] == "assistant.state"
    assert sent_messages[-2]["payload"] == {"state": "idle", "reason": "tool"}
    assert sent_messages[-1]["type"] == "device.command"
    assert sent_messages[-1]["payload"] == {"type": "end_conversation", "reason": "tool"}


def test_session_end_conversation_defers_until_turn_finishes() -> None:
    asyncio.run(_session_end_conversation_defers_until_turn_finishes())


async def _session_hello_tracks_capabilities() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-hello",
        firmware_version="0.0.1",
        capabilities=["mic", "speaker"],
    )
    manager = GatewaySessionManager(registry, _FakeHermes(), _FakeSpeech())
    session = await manager.create_session("echo-hello")

    async def _sender(_: dict) -> None:
        return None

    await manager.bind_sender(session.session_id, _sender)
    await manager.handle_control_message(
        session.session_id,
        {
            "type": "hello",
            "payload": {
                "device_id": "echo-hello",
                "capabilities": ["speaker", "mic", "audio_reference", "audio_stats", "audio_loopback"],
            },
        },
    )

    persisted = await registry.get_session(session.session_id)
    assert persisted is not None
    assert persisted.device_state["capabilities"] == [
        "speaker",
        "mic",
        "audio_reference",
        "audio_stats",
        "audio_loopback",
    ]


def test_session_manager_tracks_hello_capabilities() -> None:
    asyncio.run(_session_hello_tracks_capabilities())
