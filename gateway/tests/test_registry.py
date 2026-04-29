from hermes_iot_gateway.registry import DeviceSession, InMemoryRegistry
from hermes_iot_gateway.session import GatewaySessionManager


async def _noop_sender(_: dict) -> None:
    return None


async def _claim_and_bind() -> None:
    registry = InMemoryRegistry()
    record = await registry.claim_device(
        device_id="echo-1",
        firmware_version="0.0.1",
        capabilities=["mic"],
    )
    assert record.conversation == "iot:echo-1"
    looked_up = await registry.get_device_by_token(record.auth_token)
    assert looked_up is not None
    session = DeviceSession(session_id="sess", device_id="echo-1", conversation="iot:echo-1")
    await registry.upsert_session(session)
    bound = await registry.bind_sender("sess", _noop_sender)
    assert bound is not None
    assert bound.connected is True


def test_registry_claim_and_bind() -> None:
    import asyncio

    asyncio.run(_claim_and_bind())


class _NoopHermes:
    async def stream_text_turn(self, **kwargs):
        if False:
            yield kwargs


class _NoopSpeech:
    async def attach_audio_track(self, **kwargs):
        return None

    async def attach_output_track(self, **kwargs):
        return None

    async def detach_session(self, session):
        return None

    async def transcribe_text_payload(self, text: str):
        return None

    async def on_text_delta(self, session, text: str):
        return None

    async def on_turn_complete(self, session):
        return None


async def _session_reuse_roundtrip() -> None:
    registry = InMemoryRegistry()
    await registry.claim_device(
        device_id="echo-2",
        firmware_version="0.0.1",
        capabilities=["mic"],
    )
    manager = GatewaySessionManager(registry, _NoopHermes(), _NoopSpeech())
    session_one = await manager.create_session("echo-2")
    await registry.mark_disconnected(session_one.session_id)
    session_two = await manager.create_session("echo-2")
    assert session_one.session_id == session_two.session_id
    assert session_two.connected is True


def test_session_manager_reuses_existing_session() -> None:
    import asyncio

    asyncio.run(_session_reuse_roundtrip())
