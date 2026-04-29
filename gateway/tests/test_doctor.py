from pathlib import Path

import httpx

from hermes_iot_gateway.bootstrap import bootstrap_gateway
from hermes_iot_gateway.doctor import DoctorCheck, run_doctor


def test_doctor_treats_dev_keys_and_missing_serial_as_warnings(tmp_path: Path, monkeypatch) -> None:
    hermes_home = tmp_path / ".hermes"
    bootstrap_gateway(
        hermes_home,
        force=True,
        admin_key="dev-admin-key",
        hermes_api_key="change-me-local-dev",
        deepgram_api_key="dg-123",
    )

    def fake_post(*args, **kwargs):
        return httpx.Response(400)

    monkeypatch.setattr("hermes_iot_gateway.doctor.httpx.post", fake_post)
    monkeypatch.setattr(
        "hermes_iot_gateway.doctor._check_serial_devices",
        lambda: DoctorCheck("esp32_serial", False, "missing", required=False),
    )

    checks = run_doctor(hermes_home)
    by_name = {check.name: check for check in checks}

    assert by_name["admin_key"].ok is False
    assert by_name["admin_key"].required is False
    assert by_name["hermes_api_key"].ok is False
    assert by_name["hermes_api_key"].required is False
    assert not any(check.failed for check in checks)


def test_doctor_gateway_health_is_required_when_requested(tmp_path: Path, monkeypatch) -> None:
    hermes_home = tmp_path / ".hermes"
    bootstrap_gateway(hermes_home, force=True, deepgram_api_key="dg-123")

    monkeypatch.setattr("hermes_iot_gateway.doctor.httpx.post", lambda *args, **kwargs: httpx.Response(400))
    monkeypatch.setattr("hermes_iot_gateway.doctor.httpx.get", lambda *args, **kwargs: httpx.Response(503))
    monkeypatch.setattr(
        "hermes_iot_gateway.doctor._check_serial_devices",
        lambda: DoctorCheck("esp32_serial", False, "missing", required=False),
    )

    checks = run_doctor(hermes_home, gateway_url="http://gateway")
    by_name = {check.name: check for check in checks}

    assert by_name["gateway_health"].ok is False
    assert by_name["gateway_health"].required is True
    assert by_name["gateway_health"].failed is True
