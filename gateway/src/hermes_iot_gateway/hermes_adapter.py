from __future__ import annotations

import asyncio
import logging
import re
import uuid
from pathlib import Path
from typing import Any

import uvicorn

from .config import Settings, resolve_settings
from .models import DataChannelMessage
from .registry import DeviceSession
from .runtime import GatewayRuntime, create_app, create_native_runtime
from .spoken_text import IOT_VOICE_SYSTEM_PROMPT, sanitize_spoken_text

logger = logging.getLogger(__name__)

_MEDIA_PAUSE_PATTERN = re.compile(r"\b(?:stop|pause|quiet|silence)\b", re.IGNORECASE)
_MEDIA_NEXT_PATTERN = re.compile(r"\b(?:skip|next)\b", re.IGNORECASE)


def check_iot_requirements() -> bool:
    try:
        import aiortc  # noqa: F401
        import fastapi  # noqa: F401
        import hermes_iot_gateway  # noqa: F401
    except Exception as exc:
        logger.warning("Hermes IoT requirements are missing: %s", exc)
        return False
    return True


class IoTAdapterMixin:
    """Hermes platform adapter for WebRTC IoT devices."""

    def __init__(self, config: Any) -> None:
        from gateway.config import Platform
        from gateway.platforms.base import BasePlatformAdapter

        BasePlatformAdapter.__init__(self, config, Platform.IOT)
        self._runtime: GatewayRuntime | None = None
        self._server: uvicorn.Server | None = None
        self._server_task: asyncio.Task[None] | None = None

    async def connect(self) -> bool:
        if self._message_handler is None:
            self._set_fatal_error("iot_no_handler", "Hermes IoT adapter has no message handler", retryable=False)
            return False

        settings = self._settings_from_config()
        self._runtime = create_native_runtime(self._handle_native_turn, settings)
        app = create_app(self._runtime)
        server_config = uvicorn.Config(app, host=settings.host, port=settings.port, log_level="info")
        self._server = uvicorn.Server(server_config)
        self._server_task = asyncio.create_task(self._server.serve())
        self._mark_connected()
        logger.info("Hermes IoT gateway listening on %s:%s", settings.host, settings.port)
        return True

    async def disconnect(self) -> None:
        if self._server is not None:
            self._server.should_exit = True
        if self._server_task is not None:
            await self._server_task
        self._server = None
        self._server_task = None
        self._runtime = None
        self._mark_disconnected()

    async def send(
        self,
        chat_id: str,
        content: str,
        reply_to: str | None = None,
        metadata: dict[str, Any] | None = None,
    ):
        from gateway.platforms.base import SendResult

        session = await self._device_session(chat_id)
        if session is None:
            return SendResult(success=False, error=f"IoT device is not connected: {chat_id}")
        await self._speak_to_device(session, content)
        return SendResult(success=True, message_id=f"iot-{uuid.uuid4().hex[:12]}")

    async def send_typing(self, chat_id: str, metadata=None) -> None:
        session = await self._device_session(chat_id)
        if session is None or self._runtime is None:
            return
        await self._runtime.sessions._set_assistant_state(session, "thinking")
        await self._runtime.sessions._send(
            session,
            DataChannelMessage(type="assistant.state", payload={"state": "thinking"}),
        )

    async def get_chat_info(self, chat_id: str) -> dict[str, Any]:
        session = await self._device_session(chat_id)
        return {
            "id": chat_id,
            "type": "device",
            "title": chat_id,
            "connected": bool(session and session.connected),
        }

    async def _handle_native_turn(
        self,
        session: DeviceSession,
        turn,
        hello_metadata: dict[str, Any] | None,
    ) -> str | None:
        if self._message_handler is None:
            raise RuntimeError("Hermes message handler is not attached")

        media_response = await self._handle_media_transport_turn(session, str(turn.text or ""))
        if media_response is not None:
            return media_response

        from gateway.platforms.base import MessageEvent, MessageType
        from gateway.session import SessionSource

        source = SessionSource(
            platform=self.platform,
            chat_id=session.device_id,
            chat_name=session.device_id,
            chat_type="dm",
            user_id=session.device_id,
            user_name=session.device_id,
        )
        event = MessageEvent(
            text=turn.text,
            message_type=MessageType.VOICE,
            source=source,
            raw_message={
                "device_id": session.device_id,
                "conversation": session.conversation,
                "hello": hello_metadata or session.device_state.get("hello") or {},
                "speech": turn.metadata,
            },
            message_id=f"{session.session_id}:{uuid.uuid4().hex[:12]}",
            channel_prompt=IOT_VOICE_SYSTEM_PROMPT,
            internal=True,
        )
        self._auto_tts_disabled_chats.add(session.device_id)
        return await self._message_handler(event)

    async def _handle_media_transport_turn(self, session: DeviceSession, text: str) -> str | None:
        if self._runtime is None or not session.device_state.get("media_playing"):
            return None

        normalized = text.strip().lower()
        if not normalized:
            return None

        if _MEDIA_NEXT_PATTERN.search(normalized):
            await self._runtime.spotify.next()
            return "Skipping."

        if _MEDIA_PAUSE_PATTERN.search(normalized):
            try:
                await self._runtime.spotify.pause()
            finally:
                await self._runtime.librespot.stop()
                session.device_state["media_playing"] = False
                await self._runtime.sessions._send(
                    session,
                    DataChannelMessage(type="device.command", payload={"type": "media.mode", "playing": False}),
                )
            return "Stopped."

        return None

    async def _device_session(self, chat_id: str) -> DeviceSession | None:
        if self._runtime is None:
            return None
        return await self._runtime.registry.get_session_for_device(chat_id)

    async def _speak_to_device(self, session: DeviceSession, content: str) -> None:
        if self._runtime is None or not content:
            return
        content = sanitize_spoken_text(content)
        if not content:
            return
        await self._runtime.sessions._send(
            session,
            DataChannelMessage(type="assistant.text.delta", payload={"text": content}),
        )
        session.device_state["last_assistant_text"] = content[-2000:]
        await self._runtime.sessions._set_assistant_state(session, "speaking")
        await self._runtime.sessions._send(
            session,
            DataChannelMessage(type="assistant.state", payload={"state": "speaking"}),
        )
        await self._runtime.speech.on_text_delta(session, content)
        await self._runtime.speech.on_turn_complete(session)
        session.device_state["last_assistant_speech_ended_at"] = asyncio.get_running_loop().time()
        await self._runtime.sessions._resume_listening(session)

    def _settings_from_config(self) -> Settings:
        settings = resolve_settings()
        extra = getattr(self.config, "extra", {}) or {}
        if not isinstance(extra, dict):
            return settings

        updates: dict[str, Any] = {}
        aliases = {
            field.alias: name
            for name, field in Settings.model_fields.items()
            if field.alias is not None
        }
        for key, value in extra.items():
            field_name = key if key in Settings.model_fields else aliases.get(key)
            if field_name in Settings.model_fields:
                updates[field_name] = value
        if updates:
            settings = settings.model_copy(update=updates)
            settings.state_db_path = str(Path(settings.state_db_path).expanduser())
        return settings


def build_iot_adapter_class():
    from gateway.platforms.base import BasePlatformAdapter

    return type("IoTAdapter", (IoTAdapterMixin, BasePlatformAdapter), {})


IoTAdapter = build_iot_adapter_class()
