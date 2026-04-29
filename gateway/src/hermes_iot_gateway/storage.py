from __future__ import annotations

import asyncio
import json
import sqlite3
from pathlib import Path

from .registry import DeviceRecord, utc_now


class SQLiteStateStore:
    def __init__(self, path: str) -> None:
        self._path = Path(path)
        self._path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        await asyncio.to_thread(self._initialize_sync)

    def _initialize_sync(self) -> None:
        with sqlite3.connect(self._path) as conn:
            conn.execute(
                """
                CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY,
                    auth_token TEXT NOT NULL UNIQUE,
                    firmware_version TEXT NOT NULL,
                    capabilities_json TEXT NOT NULL,
                    paired_at TEXT NOT NULL,
                    last_seen_at TEXT NOT NULL,
                    conversation TEXT NOT NULL
                )
                """
            )
            conn.execute(
                """
                CREATE TABLE IF NOT EXISTS device_sessions (
                    device_id TEXT PRIMARY KEY,
                    session_id TEXT,
                    assistant_state TEXT NOT NULL,
                    device_state_json TEXT NOT NULL DEFAULT '{}',
                    connected INTEGER NOT NULL,
                    updated_at TEXT NOT NULL
                )
                """
            )
            columns = {
                row[1]
                for row in conn.execute("PRAGMA table_info(device_sessions)").fetchall()
            }
            if "device_state_json" not in columns:
                conn.execute(
                    "ALTER TABLE device_sessions ADD COLUMN device_state_json TEXT NOT NULL DEFAULT '{}'"
                )
            conn.commit()

    async def upsert_device(self, record: DeviceRecord) -> None:
        async with self._lock:
            await asyncio.to_thread(self._upsert_device_sync, record)

    def _upsert_device_sync(self, record: DeviceRecord) -> None:
        with sqlite3.connect(self._path) as conn:
            conn.execute(
                """
                INSERT INTO devices (
                    device_id, auth_token, firmware_version, capabilities_json, paired_at, last_seen_at, conversation
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    auth_token=excluded.auth_token,
                    firmware_version=excluded.firmware_version,
                    capabilities_json=excluded.capabilities_json,
                    paired_at=excluded.paired_at,
                    last_seen_at=excluded.last_seen_at,
                    conversation=excluded.conversation
                """,
                (
                    record.device_id,
                    record.auth_token,
                    record.firmware_version,
                    record.capabilities_json(),
                    record.paired_at.isoformat(),
                    record.last_seen_at.isoformat(),
                    record.conversation,
                ),
            )
            conn.commit()

    async def load_devices(self) -> list[DeviceRecord]:
        async with self._lock:
            return await asyncio.to_thread(self._load_devices_sync)

    def _load_devices_sync(self) -> list[DeviceRecord]:
        with sqlite3.connect(self._path) as conn:
            rows = conn.execute(
                """
                SELECT device_id, auth_token, firmware_version, capabilities_json, paired_at, last_seen_at, conversation
                FROM devices
                ORDER BY paired_at ASC
                """
            ).fetchall()
        return [DeviceRecord.from_row(*row) for row in rows]

    async def touch_device(self, device_id: str) -> None:
        async with self._lock:
            await asyncio.to_thread(self._touch_device_sync, device_id)

    def _touch_device_sync(self, device_id: str) -> None:
        with sqlite3.connect(self._path) as conn:
            conn.execute(
                "UPDATE devices SET last_seen_at = ? WHERE device_id = ?",
                (utc_now().isoformat(), device_id),
            )
            conn.commit()

    async def upsert_session_state(
        self,
        *,
        device_id: str,
        session_id: str | None,
        assistant_state: str,
        device_state: dict,
        connected: bool,
    ) -> None:
        async with self._lock:
            await asyncio.to_thread(
                self._upsert_session_state_sync,
                device_id,
                session_id,
                assistant_state,
                device_state,
                connected,
            )

    def _upsert_session_state_sync(
        self,
        device_id: str,
        session_id: str | None,
        assistant_state: str,
        device_state: dict,
        connected: bool,
    ) -> None:
        with sqlite3.connect(self._path) as conn:
            conn.execute(
                """
                INSERT INTO device_sessions (device_id, session_id, assistant_state, device_state_json, connected, updated_at)
                VALUES (?, ?, ?, ?, ?, ?)
                ON CONFLICT(device_id) DO UPDATE SET
                    session_id=excluded.session_id,
                    assistant_state=excluded.assistant_state,
                    device_state_json=excluded.device_state_json,
                    connected=excluded.connected,
                    updated_at=excluded.updated_at
                """,
                (
                    device_id,
                    session_id,
                    assistant_state,
                    json.dumps(device_state, sort_keys=True),
                    1 if connected else 0,
                    utc_now().isoformat(),
                ),
            )
            conn.commit()
