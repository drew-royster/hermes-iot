from __future__ import annotations

import asyncio
import json
import secrets
from dataclasses import dataclass, field
from datetime import UTC, datetime
from typing import Any, Callable, Awaitable


def utc_now() -> datetime:
    return datetime.now(tz=UTC)


SenderFn = Callable[[dict[str, Any]], Awaitable[None]]


@dataclass(slots=True)
class DeviceRecord:
    device_id: str
    auth_token: str
    firmware_version: str
    capabilities: list[str]
    conversation: str
    paired_at: datetime = field(default_factory=utc_now)
    last_seen_at: datetime = field(default_factory=utc_now)

    def capabilities_json(self) -> str:
        return json.dumps(self.capabilities)

    @classmethod
    def from_row(
        cls,
        device_id: str,
        auth_token: str,
        firmware_version: str,
        capabilities_json: str,
        paired_at: str,
        last_seen_at: str,
        conversation: str,
    ) -> "DeviceRecord":
        return cls(
            device_id=device_id,
            auth_token=auth_token,
            firmware_version=firmware_version,
            capabilities=json.loads(capabilities_json),
            paired_at=datetime.fromisoformat(paired_at),
            last_seen_at=datetime.fromisoformat(last_seen_at),
            conversation=conversation,
        )


@dataclass(slots=True)
class DeviceSession:
    session_id: str
    device_id: str
    conversation: str
    connected: bool = True
    capturing_audio: bool = False
    assistant_state: str = "idle"
    device_state: dict[str, Any] = field(default_factory=dict)
    sender: SenderFn | None = None
    peer: Any | None = None
    output_track: Any | None = None
    active_turn: asyncio.Task[None] | None = None


class InMemoryRegistry:
    def __init__(self) -> None:
        self._lock = asyncio.Lock()
        self._devices: dict[str, DeviceRecord] = {}
        self._tokens: dict[str, str] = {}
        self._sessions_by_device: dict[str, DeviceSession] = {}
        self._sessions_by_id: dict[str, DeviceSession] = {}

    async def claim_device(self, *, device_id: str, firmware_version: str, capabilities: list[str]) -> DeviceRecord:
        async with self._lock:
            existing = self._devices.get(device_id)
            if existing:
                self._tokens.pop(existing.auth_token, None)
            record = DeviceRecord(
                device_id=device_id,
                auth_token=secrets.token_urlsafe(24),
                firmware_version=firmware_version,
                capabilities=capabilities,
                conversation=f"iot:{device_id}",
            )
            self._devices[device_id] = record
            self._tokens[record.auth_token] = device_id
            return record

    async def get_device_by_token(self, token: str) -> DeviceRecord | None:
        async with self._lock:
            device_id = self._tokens.get(token)
            if not device_id:
                return None
            record = self._devices.get(device_id)
            if record:
                record.last_seen_at = utc_now()
            return record

    async def get_device(self, device_id: str) -> DeviceRecord | None:
        async with self._lock:
            return self._devices.get(device_id)

    async def upsert_session(self, session: DeviceSession) -> DeviceSession:
        async with self._lock:
            existing = self._sessions_by_device.get(session.device_id)
            if (
                existing
                and existing.session_id != session.session_id
                and existing.active_turn
                and not existing.active_turn.done()
            ):
                existing.active_turn.cancel()
            self._sessions_by_device[session.device_id] = session
            self._sessions_by_id[session.session_id] = session
            return session

    async def get_session_for_device(self, device_id: str) -> DeviceSession | None:
        async with self._lock:
            return self._sessions_by_device.get(device_id)

    async def get_session(self, session_id: str) -> DeviceSession | None:
        async with self._lock:
            return self._sessions_by_id.get(session_id)

    async def mark_disconnected(self, session_id: str) -> None:
        async with self._lock:
            session = self._sessions_by_id.get(session_id)
            if session:
                session.connected = False
                session.assistant_state = "idle"

    async def bind_sender(self, session_id: str, sender: SenderFn) -> DeviceSession | None:
        async with self._lock:
            session = self._sessions_by_id.get(session_id)
            if not session:
                return None
            session.sender = sender
            session.connected = True
            return session

    async def list_devices(self) -> list[tuple[DeviceRecord, DeviceSession | None]]:
        async with self._lock:
            return [
                (record, self._sessions_by_device.get(record.device_id))
                for record in self._devices.values()
            ]


class PersistentRegistry(InMemoryRegistry):
    def __init__(self, store) -> None:
        super().__init__()
        self._store = store

    async def initialize(self) -> None:
        await self._store.initialize()
        self._devices.clear()
        self._tokens.clear()
        for record in await self._store.load_devices():
            self._devices[record.device_id] = record
            self._tokens[record.auth_token] = record.device_id

    async def claim_device(self, *, device_id: str, firmware_version: str, capabilities: list[str]) -> DeviceRecord:
        record = await super().claim_device(
            device_id=device_id,
            firmware_version=firmware_version,
            capabilities=capabilities,
        )
        await self._store.upsert_device(record)
        return record

    async def get_device_by_token(self, token: str) -> DeviceRecord | None:
        record = await super().get_device_by_token(token)
        if record:
            await self._store.touch_device(record.device_id)
        return record

    async def upsert_session(self, session: DeviceSession) -> DeviceSession:
        result = await super().upsert_session(session)
        await self._store.upsert_session_state(
            device_id=session.device_id,
            session_id=session.session_id,
            assistant_state=session.assistant_state,
            device_state=session.device_state,
            connected=session.connected,
        )
        return result

    async def mark_disconnected(self, session_id: str) -> None:
        session = await self.get_session(session_id)
        await super().mark_disconnected(session_id)
        if session:
            await self._store.upsert_session_state(
                device_id=session.device_id,
                session_id=session.session_id,
                assistant_state="idle",
                device_state=session.device_state,
                connected=False,
            )

    async def bind_sender(self, session_id: str, sender: SenderFn) -> DeviceSession | None:
        session = await super().bind_sender(session_id, sender)
        if session:
            await self._store.upsert_session_state(
                device_id=session.device_id,
                session_id=session.session_id,
                assistant_state=session.assistant_state,
                device_state=session.device_state,
                connected=True,
            )
        return session
