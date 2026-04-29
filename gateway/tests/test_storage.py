import asyncio
from pathlib import Path

from hermes_iot_gateway.registry import PersistentRegistry
from hermes_iot_gateway.storage import SQLiteStateStore


async def _roundtrip(tmp_path: Path) -> None:
    store = SQLiteStateStore(str(tmp_path / "state.db"))
    registry = PersistentRegistry(store)
    await registry.initialize()
    claimed = await registry.claim_device(
        device_id="echo-persist",
        firmware_version="0.0.2",
        capabilities=["mic", "speaker"],
    )
    assert claimed.conversation == "iot:echo-persist"

    second_registry = PersistentRegistry(store)
    await second_registry.initialize()
    loaded = await second_registry.get_device("echo-persist")
    assert loaded is not None
    assert loaded.auth_token == claimed.auth_token
    assert loaded.capabilities == ["mic", "speaker"]


def test_sqlite_device_persistence(tmp_path: Path) -> None:
    asyncio.run(_roundtrip(tmp_path))
