from __future__ import annotations

import argparse
import os
import secrets
from dataclasses import dataclass
from pathlib import Path

from .installer import install_plugin
from .profile import default_hermes_home, write_gateway_env, write_gateway_profile


@dataclass(slots=True)
class BootstrapResult:
    hermes_home: Path
    plugin_path: Path
    profile_path: Path
    env_path: Path


def _profile_exists(hermes_home: Path) -> bool:
    return (hermes_home / "hermes_iot.yaml").exists() or (hermes_home / "hermes_iot.env").exists()


def bootstrap_gateway(
    hermes_home: Path,
    *,
    force: bool = False,
    install_plugin_assets: bool = True,
    host: str = "0.0.0.0",
    port: int = 8787,
    state_db_path: str | None = None,
    hermes_api_base_url: str = "http://127.0.0.1:8642/v1",
    hermes_model: str = "hermes-agent",
    speech_to_text_provider: str = "deepgram",
    text_to_speech_provider: str = "deepgram",
    admin_key: str | None = None,
    hermes_api_key: str | None = None,
    deepgram_api_key: str | None = None,
) -> BootstrapResult:
    resolved_home = Path(hermes_home).expanduser()
    if _profile_exists(resolved_home) and not force:
        raise FileExistsError(
            f"Hermes IoT config already exists in {resolved_home}. Use --force to replace it."
        )

    db_path = state_db_path or str((resolved_home / "iot" / "hermes_iot.db").expanduser())
    Path(db_path).expanduser().parent.mkdir(parents=True, exist_ok=True)
    profile = {
        "iot_gateway": {
            "host": host,
            "port": port,
            "state_db_path": db_path,
            "hermes_api_base_url": hermes_api_base_url,
            "hermes_model": hermes_model,
            "speech_to_text_provider": speech_to_text_provider,
            "text_to_speech_provider": text_to_speech_provider,
            "default_ice_servers": ["stun:stun.l.google.com:19302"],
            "wake_word_enabled": False,
            "wake_word": "hey willow",
            "sleep_timeout_seconds": 30.0,
        }
    }
    env_values = {
        "HERMES_IOT_ADMIN_KEY": admin_key or os.environ.get("HERMES_IOT_ADMIN_KEY") or secrets.token_urlsafe(24),
        "HERMES_API_KEY": hermes_api_key or os.environ.get("HERMES_API_KEY") or "change-me-local-dev",
    }
    resolved_deepgram_key = deepgram_api_key or os.environ.get("DEEPGRAM_API_KEY")
    if resolved_deepgram_key:
        env_values["DEEPGRAM_API_KEY"] = resolved_deepgram_key

    if install_plugin_assets:
        plugin_path = install_plugin(resolved_home, force=force)
    else:
        plugin_path = resolved_home / "plugins" / "hermes_iot"

    profile_path = write_gateway_profile(resolved_home, profile=profile)
    env_path = write_gateway_env(resolved_home, values=env_values)
    return BootstrapResult(
        hermes_home=resolved_home,
        plugin_path=plugin_path,
        profile_path=profile_path,
        env_path=env_path,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Set up Hermes IoT inside a Hermes profile")
    parser.add_argument("--hermes-home", default=str(default_hermes_home()))
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--skip-plugin-install", action="store_true")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--state-db-path", default=None)
    parser.add_argument("--hermes-api-base-url", default="http://127.0.0.1:8642/v1")
    parser.add_argument("--hermes-model", default="hermes-agent")
    parser.add_argument("--stt-provider", default="deepgram")
    parser.add_argument("--tts-provider", default="deepgram")
    parser.add_argument("--admin-key", default=None)
    parser.add_argument("--hermes-api-key", default=None)
    parser.add_argument("--deepgram-api-key", default=None)
    args = parser.parse_args()

    result = bootstrap_gateway(
        Path(args.hermes_home),
        force=args.force,
        install_plugin_assets=not args.skip_plugin_install,
        host=args.host,
        port=args.port,
        state_db_path=args.state_db_path,
        hermes_api_base_url=args.hermes_api_base_url,
        hermes_model=args.hermes_model,
        speech_to_text_provider=args.stt_provider,
        text_to_speech_provider=args.tts_provider,
        admin_key=args.admin_key,
        hermes_api_key=args.hermes_api_key,
        deepgram_api_key=args.deepgram_api_key,
    )

    print(f"Hermes home: {result.hermes_home}")
    print(f"Plugin: {result.plugin_path}")
    print(f"Profile: {result.profile_path}")
    print(f"Env: {result.env_path}")
