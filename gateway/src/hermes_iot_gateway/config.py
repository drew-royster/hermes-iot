from __future__ import annotations

from pathlib import Path
from typing import Any, Literal

from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict

from .profile import gateway_env_path, gateway_profile_path, load_gateway_profile
from .spoken_text import IOT_VOICE_SYSTEM_PROMPT


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")

    host: str = Field(default="127.0.0.1", alias="HERMES_IOT_HOST")
    port: int = Field(default=8787, alias="HERMES_IOT_PORT")
    admin_key: str = Field(default="dev-admin-key", alias="HERMES_IOT_ADMIN_KEY")
    state_db_path: str = Field(default="./var/hermes_iot.db", alias="HERMES_IOT_STATE_DB")
    hermes_api_base_url: str = Field(default="http://127.0.0.1:8642/v1", alias="HERMES_API_BASE_URL")
    hermes_api_key: str = Field(default="change-me-local-dev", alias="HERMES_API_KEY")
    hermes_model: str = Field(default="hermes-agent", alias="HERMES_API_MODEL")
    conversation_mode: Literal["session", "device"] = Field(default="session", alias="HERMES_IOT_CONVERSATION_MODE")
    default_instructions: str = Field(
        default=(
            "You are speaking through a Hermes IoT voice gateway attached to an embedded desk device. "
            "For normal conversation, answer in one short spoken sentence. "
            "Be direct, interruptible, and avoid filler unless the user asks for detail. "
            f"{IOT_VOICE_SYSTEM_PROMPT}"
        ),
        alias="HERMES_IOT_DEFAULT_INSTRUCTIONS",
    )
    default_ice_servers: list[str] = Field(default_factory=lambda: ["stun:stun.l.google.com:19302"])
    speech_to_text_provider: str = Field(default="deepgram", alias="HERMES_IOT_STT_PROVIDER")
    text_to_speech_provider: str = Field(default="deepgram", alias="HERMES_IOT_TTS_PROVIDER")
    deepgram_api_key: str | None = Field(default=None, alias="DEEPGRAM_API_KEY")
    deepgram_flux_model: str = Field(default="flux-general-en", alias="HERMES_IOT_DEEPGRAM_FLUX_MODEL")
    deepgram_flux_sample_rate: int = Field(default=16000, alias="HERMES_IOT_DEEPGRAM_FLUX_SAMPLE_RATE")
    deepgram_flux_eot_threshold: float = Field(default=0.7, alias="HERMES_IOT_DEEPGRAM_FLUX_EOT_THRESHOLD")
    deepgram_flux_eager_eot_threshold: float = Field(default=0.7, alias="HERMES_IOT_DEEPGRAM_FLUX_EAGER_EOT_THRESHOLD")
    deepgram_flux_eot_timeout_ms: int = Field(default=1800, alias="HERMES_IOT_DEEPGRAM_FLUX_EOT_TIMEOUT_MS")
    deepgram_aura_model: str = Field(default="aura-2-thalia-en", alias="HERMES_IOT_DEEPGRAM_AURA_MODEL")
    deepgram_aura_sample_rate: int = Field(default=48000, alias="HERMES_IOT_DEEPGRAM_AURA_SAMPLE_RATE")
    # Firmware-local wake is the normal Echo Pyramid path. Keep gateway-side
    # text wake gating available for simulated or always-streaming clients.
    wake_word_enabled: bool = Field(default=False, alias="HERMES_IOT_WAKE_WORD_ENABLED")
    wake_word: str = Field(default="hey willow", alias="HERMES_IOT_WAKE_WORD")
    sleep_timeout_seconds: float = Field(default=30.0, alias="HERMES_IOT_SLEEP_TIMEOUT_SECONDS")
    librespot_enabled: bool = Field(default=False, alias="HERMES_IOT_LIBRESPOT_ENABLED")
    librespot_command: str = Field(default="librespot", alias="HERMES_IOT_LIBRESPOT_COMMAND")
    librespot_name: str = Field(default="Hermes Echo Pyramid", alias="HERMES_IOT_LIBRESPOT_NAME")
    librespot_backend: str = Field(default="pipe", alias="HERMES_IOT_LIBRESPOT_BACKEND")
    librespot_device: str | None = Field(default=None, alias="HERMES_IOT_LIBRESPOT_DEVICE")
    librespot_target_device_id: str | None = Field(default=None, alias="HERMES_IOT_LIBRESPOT_TARGET_DEVICE_ID")
    librespot_cache_dir: str = Field(default="~/.hermes/iot/librespot", alias="HERMES_IOT_LIBRESPOT_CACHE_DIR")
    librespot_initial_volume: int = Field(default=80, alias="HERMES_IOT_LIBRESPOT_INITIAL_VOLUME")
    librespot_bitrate: int = Field(default=320, alias="HERMES_IOT_LIBRESPOT_BITRATE")
    librespot_output_sample_rate: int = Field(default=48000, alias="HERMES_IOT_LIBRESPOT_OUTPUT_SAMPLE_RATE")
    librespot_buffer_ms: int = Field(default=1500, alias="HERMES_IOT_LIBRESPOT_BUFFER_MS")
    spotify_client_id: str | None = Field(default=None, alias="HERMES_IOT_SPOTIFY_CLIENT_ID")
    spotify_redirect_uri: str = Field(
        default="http://127.0.0.1:8787/v1/music/spotify/callback",
        alias="HERMES_IOT_SPOTIFY_REDIRECT_URI",
    )
    spotify_token_path: str = Field(default="~/.hermes/iot/spotify_auth.json", alias="HERMES_IOT_SPOTIFY_TOKEN_PATH")
    spotify_device_name: str = Field(default="Hermes Echo Pyramid", alias="HERMES_IOT_SPOTIFY_DEVICE_NAME")
    spotify_device_id: str | None = Field(default=None, alias="HERMES_IOT_SPOTIFY_DEVICE_ID")


def _field_default(field_name: str) -> Any:
    field = Settings.model_fields[field_name]
    if field.default_factory is not None:
        return field.default_factory()
    return field.default


def _normalize_profile_values(data: dict[str, Any]) -> dict[str, Any]:
    alias_to_name = {
        field.alias: name
        for name, field in Settings.model_fields.items()
        if field.alias is not None
    }
    normalized: dict[str, Any] = {}
    for key, value in data.items():
        field_name = key if key in Settings.model_fields else alias_to_name.get(key)
        if field_name is None:
            continue
        normalized[field_name] = value
    return normalized


def _profile_env_updates(env_values: dict[str, str]) -> dict[str, Any]:
    if not env_values:
        return {}
    model = Settings.model_validate(env_values)
    return {
        field_name: getattr(model, field_name)
        for field_name in model.model_fields_set
        if field_name in Settings.model_fields
    }


def resolve_settings(
    settings: Settings | None = None,
    *,
    hermes_home: Path | None = None,
) -> Settings:
    if settings is not None:
        settings.state_db_path = str(Path(settings.state_db_path).expanduser())
        return settings

    has_hermes_profile = gateway_profile_path(hermes_home).exists() or gateway_env_path(hermes_home).exists()
    resolved = Settings(_env_file=None) if has_hermes_profile else Settings()
    profile = load_gateway_profile(hermes_home)

    updates: dict[str, Any] = {}
    for source in (_normalize_profile_values(profile.data), _profile_env_updates(profile.env)):
        for field_name, value in source.items():
            if field_name in resolved.model_fields_set:
                continue
            if getattr(resolved, field_name) != _field_default(field_name):
                continue
            updates[field_name] = value

    merged = resolved.model_copy(update=updates)
    merged.state_db_path = str(Path(merged.state_db_path).expanduser())
    return merged
