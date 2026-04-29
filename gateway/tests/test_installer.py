from pathlib import Path

import yaml

from hermes_iot_gateway.installer import install_native_gateway, install_plugin


def test_install_plugin_copies_assets(tmp_path: Path) -> None:
    destination = install_plugin(tmp_path, force=False)
    assert destination.exists()
    assert (destination / "plugin.yaml").exists()
    assert (destination / "__init__.py").exists()
    assert (destination / "skills" / "device-session" / "SKILL.md").exists()
    config = yaml.safe_load((tmp_path / "config.yaml").read_text())
    assert "hermes_iot" in config["plugins"]["enabled"]


def test_install_native_gateway_patches_local_hermes_checkout(tmp_path: Path) -> None:
    hermes_home = tmp_path / "home"
    hermes_root = tmp_path / "hermes-agent"
    gateway_dir = hermes_root / "gateway"
    platform_dir = gateway_dir / "platforms"
    platform_dir.mkdir(parents=True)
    (gateway_dir / "config.py").write_text(
        'class Platform:\n'
        '    API_SERVER = "api_server"\n'
        "\n"
        "def get_connected_platforms(platform, connected):\n"
        "            elif platform == Platform.API_SERVER:\n"
        "                connected.append(platform)\n"
    )
    (gateway_dir / "run.py").write_text(
        "def create(platform, config, logger):\n"
        "        elif platform == Platform.API_SERVER:\n"
        "            from gateway.platforms.api_server import APIServerAdapter, check_api_server_requirements\n"
        "            if not check_api_server_requirements():\n"
        "                logger.warning(\"API Server: aiohttp not installed\")\n"
        "                return None\n"
        "            return APIServerAdapter(config)\n"
        "\n"
        "        if not history and source.platform and source.platform != Platform.LOCAL and source.platform != Platform.WEBHOOK:\n"
        "            pass\n"
        "\n"
        "async def _handle_set_home_command(self, event):\n"
        "        source = event.source\n"
        "        platform_name = source.platform.value if source.platform else \"unknown\"\n"
        "        chat_id = source.chat_id\n"
        "        return chat_id\n"
        "\n"
        "agent = AIAgent(\n"
        "                    enabled_toolsets=enabled_toolsets,\n"
        "                    ephemeral_system_prompt=combined_ephemeral or None,\n"
        ")\n"
    )

    result = install_native_gateway(hermes_home, hermes_agent_root=hermes_root, install_dependencies=False)

    assert (platform_dir / "iot.py").exists()
    assert result["python_path"] is None
    assert result["dependencies"] is False
    assert result["config_py"] is True
    assert result["run_py"] is True
    run_py = (gateway_dir / "run.py").read_text()
    assert "skip_memory=(source.platform == Platform.IOT)" in run_py
    assert "source.platform not in {Platform.WEBHOOK, Platform.IOT}" in run_py
    assert "IoT voice devices cannot be set as a Hermes home channel." in run_py
    config = yaml.safe_load((hermes_home / "config.yaml").read_text())
    assert config["platforms"]["iot"]["enabled"] is True
    assert config["platform_toolsets"]["iot"] == ["hermes-iot", "no_mcp"]
    assert "hermes_iot" in config["plugins"]["enabled"]
