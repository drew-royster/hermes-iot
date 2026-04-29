from pathlib import Path

from hermes_iot_gateway.bootstrap import bootstrap_gateway
from hermes_iot_gateway.config import resolve_settings
from hermes_iot_gateway.profile import gateway_env_path, gateway_profile_path


def test_bootstrap_writes_hermes_profile_and_env(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.chdir(tmp_path)
    hermes_home = tmp_path / ".hermes"

    result = bootstrap_gateway(
        hermes_home,
        force=True,
        host="0.0.0.0",
        port=9797,
        hermes_api_base_url="http://127.0.0.1:9999/v1",
        admin_key="admin-123",
        hermes_api_key="hermes-123",
        deepgram_api_key="dg-123",
    )

    assert result.profile_path == gateway_profile_path(hermes_home)
    assert result.env_path == gateway_env_path(hermes_home)
    assert result.plugin_path.exists()

    settings = resolve_settings(hermes_home=hermes_home)
    assert settings.host == "0.0.0.0"
    assert settings.port == 9797
    assert settings.hermes_api_base_url == "http://127.0.0.1:9999/v1"
    assert settings.admin_key == "admin-123"
    assert settings.hermes_api_key == "hermes-123"
    assert settings.deepgram_api_key == "dg-123"
    assert settings.state_db_path == str((hermes_home / "iot" / "hermes_iot.db").expanduser())
    assert Path(settings.state_db_path).parent.exists()


def test_explicit_environment_wins_over_hermes_profile(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.chdir(tmp_path)
    hermes_home = tmp_path / ".hermes"
    bootstrap_gateway(
        hermes_home,
        force=True,
        port=9797,
        admin_key="admin-123",
        hermes_api_key="hermes-123",
    )

    monkeypatch.setenv("HERMES_IOT_PORT", "8787")
    settings = resolve_settings(hermes_home=hermes_home)
    assert settings.port == 8787
