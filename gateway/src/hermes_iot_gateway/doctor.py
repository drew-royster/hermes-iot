from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

import httpx

from .config import resolve_settings
from .profile import default_hermes_home, gateway_env_path, gateway_profile_path, plugin_install_path


@dataclass(slots=True)
class DoctorCheck:
    name: str
    ok: bool
    detail: str
    required: bool = True

    @property
    def failed(self) -> bool:
        return self.required and not self.ok


def _check_plugin(hermes_home: Path) -> DoctorCheck:
    path = plugin_install_path(hermes_home)
    ok = path.exists() and (path / "plugin.yaml").exists()
    return DoctorCheck("plugin", ok, str(path))


def _check_profile_files(hermes_home: Path) -> list[DoctorCheck]:
    profile_path = gateway_profile_path(hermes_home)
    env_path = gateway_env_path(hermes_home)
    return [
        DoctorCheck("profile", profile_path.exists(), str(profile_path)),
        DoctorCheck("env", env_path.exists(), str(env_path)),
    ]


def _check_keys(settings) -> list[DoctorCheck]:
    return [
        DoctorCheck(
            "admin_key",
            settings.admin_key != "dev-admin-key",
            "configured" if settings.admin_key != "dev-admin-key" else "still using dev-admin-key",
            required=False,
        ),
        DoctorCheck(
            "hermes_api_key",
            settings.hermes_api_key != "change-me-local-dev",
            "configured" if settings.hermes_api_key != "change-me-local-dev" else "still using change-me-local-dev",
            required=False,
        ),
        DoctorCheck(
            "deepgram_api_key",
            bool(settings.deepgram_api_key),
            "configured" if settings.deepgram_api_key else "missing",
        ),
    ]


def _check_hermes(settings) -> DoctorCheck:
    url = f"{settings.hermes_api_base_url.rstrip('/')}/responses"
    headers = {"Authorization": f"Bearer {settings.hermes_api_key}"}
    try:
        response = httpx.post(url, headers=headers, json={}, timeout=5.0)
    except Exception as exc:
        return DoctorCheck("hermes_api", False, f"{url} ({exc})")
    ok = response.status_code in {200, 400, 401, 403, 422}
    return DoctorCheck("hermes_api", ok, f"{url} -> {response.status_code}")


def _check_serial_devices() -> DoctorCheck:
    patterns = ("/dev/cu.usbmodem*", "/dev/cu.usbserial*", "/dev/cu.SLAB*")
    devices: list[str] = []
    for pattern in patterns:
        devices.extend(str(path) for path in sorted(Path("/").glob(pattern.removeprefix("/"))))
    return DoctorCheck(
        "esp32_serial",
        bool(devices),
        ", ".join(devices) if devices else "no ESP32 serial device visible",
        required=False,
    )


def _check_gateway_health(gateway_url: str) -> DoctorCheck:
    url = f"{gateway_url.rstrip('/')}/health"
    try:
        response = httpx.get(url, timeout=3.0)
    except Exception as exc:
        return DoctorCheck("gateway_health", False, f"{url} ({exc})")
    return DoctorCheck("gateway_health", response.status_code == 200, f"{url} -> {response.status_code}")


def run_doctor(hermes_home: Path, *, gateway_url: str | None = None) -> list[DoctorCheck]:
    settings = resolve_settings(hermes_home=hermes_home)
    checks: list[DoctorCheck] = []
    checks.append(_check_plugin(hermes_home))
    checks.extend(_check_profile_files(hermes_home))
    checks.extend(_check_keys(settings))
    checks.append(
        DoctorCheck(
            "state_db",
            Path(settings.state_db_path).expanduser().parent.exists(),
            str(Path(settings.state_db_path).expanduser()),
        )
    )
    checks.append(_check_hermes(settings))
    checks.append(_check_serial_devices())
    if gateway_url:
        checks.append(_check_gateway_health(gateway_url))
    return checks


def main() -> None:
    parser = argparse.ArgumentParser(description="Check whether Hermes IoT is installed cleanly")
    parser.add_argument("--hermes-home", default=str(default_hermes_home()))
    parser.add_argument(
        "--gateway-url",
        default=None,
        help="Also require a running gateway health check, for example http://127.0.0.1:8787",
    )
    args = parser.parse_args()

    checks = run_doctor(Path(args.hermes_home), gateway_url=args.gateway_url)
    failures = False
    for check in checks:
        status = "ok" if check.ok else "fail" if check.required else "warn"
        print(f"[{status}] {check.name}: {check.detail}")
        failures = failures or check.failed
    if failures:
        raise SystemExit(1)
