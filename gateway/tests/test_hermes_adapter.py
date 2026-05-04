import asyncio
import sys
import types

gateway_module = types.ModuleType("gateway")
platforms_module = types.ModuleType("gateway.platforms")
base_module = types.ModuleType("gateway.platforms.base")
base_module.BasePlatformAdapter = type("BasePlatformAdapter", (), {})
sys.modules.setdefault("gateway", gateway_module)
sys.modules.setdefault("gateway.platforms", platforms_module)
sys.modules.setdefault("gateway.platforms.base", base_module)

from hermes_iot_gateway.hermes_adapter import IoTAdapterMixin
from hermes_iot_gateway.registry import DeviceSession


class _FakeSpotify:
    def __init__(self) -> None:
        self.paused = 0
        self.nexted = 0

    async def pause(self) -> dict[str, bool]:
        self.paused += 1
        return {"accepted": True}

    async def next(self) -> dict[str, bool]:
        self.nexted += 1
        return {"accepted": True}


class _FakeLibrespot:
    def __init__(self) -> None:
        self.stopped = 0

    async def stop(self) -> dict[str, object]:
        self.stopped += 1
        return {"running": False}


class _FakeSessions:
    def __init__(self) -> None:
        self.sent: list[dict] = []

    async def _send(self, session, message) -> None:
        self.sent.append(message.model_dump(mode="json"))


class _FakeRuntime:
    def __init__(self) -> None:
        self.spotify = _FakeSpotify()
        self.librespot = _FakeLibrespot()
        self.sessions = _FakeSessions()


async def _media_stop_phrase_short_circuits_agent() -> None:
    adapter = object.__new__(IoTAdapterMixin)
    runtime = _FakeRuntime()
    adapter._runtime = runtime
    session = DeviceSession(
        session_id="session",
        device_id="echo-pyramid-dev",
        conversation="iot:echo-pyramid-dev:session",
        device_state={"media_playing": True},
    )

    response = await adapter._handle_media_transport_turn(session, "Can you stop and eat it?")

    assert response == "Stopped."
    assert runtime.spotify.paused == 1
    assert runtime.librespot.stopped == 1
    assert session.device_state["media_playing"] is False
    assert runtime.sessions.sent[0]["type"] == "device.command"
    assert runtime.sessions.sent[0]["payload"] == {"type": "media.mode", "playing": False}


def test_media_stop_phrase_short_circuits_agent() -> None:
    asyncio.run(_media_stop_phrase_short_circuits_agent())


async def _non_media_stop_phrase_falls_through() -> None:
    adapter = object.__new__(IoTAdapterMixin)
    runtime = _FakeRuntime()
    adapter._runtime = runtime
    session = DeviceSession(
        session_id="session",
        device_id="echo-pyramid-dev",
        conversation="iot:echo-pyramid-dev:session",
        device_state={},
    )

    response = await adapter._handle_media_transport_turn(session, "stop")

    assert response is None
    assert runtime.spotify.paused == 0
    assert runtime.librespot.stopped == 0


def test_non_media_stop_phrase_falls_through() -> None:
    asyncio.run(_non_media_stop_phrase_falls_through())


async def _media_next_phrase_short_circuits_agent() -> None:
    adapter = object.__new__(IoTAdapterMixin)
    runtime = _FakeRuntime()
    adapter._runtime = runtime
    session = DeviceSession(
        session_id="session",
        device_id="echo-pyramid-dev",
        conversation="iot:echo-pyramid-dev:session",
        device_state={"media_playing": True},
    )

    response = await adapter._handle_media_transport_turn(session, "skip this song")

    assert response == "Skipping."
    assert runtime.spotify.nexted == 1
    assert runtime.librespot.stopped == 0


def test_media_next_phrase_short_circuits_agent() -> None:
    asyncio.run(_media_next_phrase_short_circuits_agent())
