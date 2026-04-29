from __future__ import annotations

import os
from dataclasses import dataclass
from json import loads as json_loads
from pathlib import Path
from typing import Any

import yaml


def default_hermes_home() -> Path:
    raw = os.environ.get("HERMES_HOME")
    if raw:
        return Path(raw).expanduser()
    return Path.home() / ".hermes"


def _resolve_home(hermes_home: Path | str | None = None) -> Path:
    if hermes_home is None:
        return default_hermes_home()
    return Path(hermes_home).expanduser()


def gateway_profile_path(hermes_home: Path | str | None = None) -> Path:
    return _resolve_home(hermes_home) / "hermes_iot.yaml"


def gateway_env_path(hermes_home: Path | str | None = None) -> Path:
    return _resolve_home(hermes_home) / "hermes_iot.env"


def plugin_install_path(hermes_home: Path | str | None = None) -> Path:
    return _resolve_home(hermes_home) / "plugins" / "hermes_iot"


def _read_yaml_file(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    loaded = yaml.safe_load(path.read_text(encoding="utf-8"))
    if isinstance(loaded, dict):
        return loaded
    return {}


def _read_env_file(path: Path) -> dict[str, str]:
    if not path.exists():
        return {}
    data: dict[str, str] = {}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        data[key.strip()] = value.strip().strip("\"'")
    return data


def _write_env_file(path: Path, values: dict[str, str]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"{key}={value}" for key, value in values.items()]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    try:
        path.chmod(0o600)
    except OSError:
        pass
    return path


def load_honcho_config(hermes_home: Path | str | None = None) -> dict[str, Any]:
    path = _resolve_home(hermes_home) / "honcho.json"
    if not path.exists():
        return {}
    try:
        return json_loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}


@dataclass(slots=True)
class HermesGatewayProfile:
    hermes_home: Path
    profile_path: Path
    env_path: Path
    data: dict[str, Any]
    env: dict[str, str]

    def get(self, *keys: str) -> Any:
        current: Any = self.data
        for key in keys:
            if not isinstance(current, dict):
                return None
            current = current.get(key)
        return current


def load_gateway_profile(hermes_home: Path | str | None = None) -> HermesGatewayProfile:
    resolved_home = _resolve_home(hermes_home)
    config_data = _read_yaml_file(resolved_home / "config.yaml")
    profile_data = _read_yaml_file(gateway_profile_path(resolved_home))
    merged: dict[str, Any] = {}

    if isinstance(config_data.get("iot_gateway"), dict):
        merged.update(config_data["iot_gateway"])
    if isinstance(profile_data.get("iot_gateway"), dict):
        merged.update(profile_data["iot_gateway"])
    elif profile_data:
        merged.update(profile_data)

    return HermesGatewayProfile(
        hermes_home=resolved_home,
        profile_path=gateway_profile_path(resolved_home),
        env_path=gateway_env_path(resolved_home),
        data=merged,
        env=_read_env_file(gateway_env_path(resolved_home)),
    )


def write_gateway_profile(
    hermes_home: Path,
    *,
    profile: dict[str, Any],
) -> Path:
    path = gateway_profile_path(hermes_home)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        yaml.safe_dump(profile, sort_keys=False),
        encoding="utf-8",
    )
    try:
        path.chmod(0o600)
    except OSError:
        pass
    return path


def write_gateway_env(hermes_home: Path, *, values: dict[str, str]) -> Path:
    return _write_env_file(gateway_env_path(hermes_home), values)
