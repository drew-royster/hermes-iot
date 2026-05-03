from __future__ import annotations

from datetime import UTC, datetime
from typing import Any, Literal

from pydantic import BaseModel, Field


def utc_now() -> datetime:
    return datetime.now(tz=UTC)


class DeviceHello(BaseModel):
    device_id: str
    firmware_version: str
    transport: Literal["webrtc"] = "webrtc"
    sample_rate_hz: int | None = 16000
    codec: str | None = "pcm16"
    capabilities: list[str] = Field(default_factory=list)
    metadata: dict[str, Any] = Field(default_factory=dict)


class PairClaimRequest(BaseModel):
    device_id: str
    firmware_version: str
    capabilities: list[str] = Field(default_factory=list)


class PairClaimResponse(BaseModel):
    device_id: str
    auth_token: str
    signaling_url: str
    expires_at: datetime
    conversation: str


class WebRTCOfferRequest(BaseModel):
    sdp: str
    type: Literal["offer"]
    hello: DeviceHello


class IceCandidateRequest(BaseModel):
    candidate: str
    sdp_mid: str | None = None
    sdp_mline_index: int | None = None


class IceServerConfig(BaseModel):
    urls: list[str]


class WebRTCOfferResponse(BaseModel):
    type: Literal["answer"]
    sdp: str
    session_id: str
    conversation: str
    ice_servers: list[IceServerConfig]


class InterruptRequest(BaseModel):
    reason: str = "user"


class DeviceCommandRequest(BaseModel):
    type: Literal[
        "set_led",
        "beep",
        "display_text",
        "audio_loopback",
        "audio.test",
        "audio.aec_probe",
        "media.mode",
        "end_conversation",
    ]
    payload: dict[str, Any] = Field(default_factory=dict)


class DebugPlaybackRequest(BaseModel):
    kind: Literal["tone", "file"] = "tone"
    path: str | None = None
    frequency_hz: int = 440
    duration_ms: int = 1200
    gain: float = 0.25
    sample_rate_hz: int = 16000


class SpotifyPlayRequest(BaseModel):
    query: str | None = None
    uri: str | None = None
    device_id: str | None = None


class SpotifyVolumeRequest(BaseModel):
    percent: int = Field(ge=0, le=100)


class DeviceSummary(BaseModel):
    device_id: str
    paired_at: datetime
    last_seen_at: datetime
    firmware_version: str
    capabilities: list[str]
    connected: bool
    conversation: str
    session_id: str | None = None
    device_state: dict[str, Any] = Field(default_factory=dict)


class DataChannelMessage(BaseModel):
    type: Literal[
        "hello",
        "device.state",
        "assistant.state",
        "assistant.text.delta",
        "tool.progress",
        "audio.input.level",
        "audio.stats",
        "debug.user_text",
        "interrupt",
        "mute.set",
        "volume.set",
        "device.command",
        "error",
    ]
    payload: dict[str, Any] = Field(default_factory=dict)
    timestamp: datetime = Field(default_factory=utc_now)


class SpeechTurn(BaseModel):
    text: str
    source: Literal["debug_text", "stt"]
    metadata: dict[str, Any] = Field(default_factory=dict)


class HermesStreamEvent(BaseModel):
    kind: Literal[
        "response.created",
        "assistant.text.delta",
        "tool.call",
        "tool.output",
        "response.completed",
        "response.failed",
    ]
    payload: dict[str, Any] = Field(default_factory=dict)
