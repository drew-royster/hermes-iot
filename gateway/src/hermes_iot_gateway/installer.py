from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

import yaml

from .profile import default_hermes_home

PLUGIN_KEY = "hermes_iot"


def _repo_plugin_source() -> Path:
    return Path(__file__).resolve().parents[3] / "plugin" / "hermes_iot"


def _ensure_plugin_enabled(data: dict) -> bool:
    plugins = data.setdefault("plugins", {})
    if not isinstance(plugins, dict):
        plugins = {}
        data["plugins"] = plugins
    enabled = plugins.setdefault("enabled", [])
    if not isinstance(enabled, list):
        enabled = []
        plugins["enabled"] = enabled
    if PLUGIN_KEY not in enabled:
        enabled.append(PLUGIN_KEY)
        return True
    return False


def _enable_plugin_config(hermes_home: Path) -> bool:
    config_path = hermes_home / "config.yaml"
    data = yaml.safe_load(config_path.read_text()) if config_path.exists() else {}
    if not isinstance(data, dict):
        data = {}

    before = yaml.safe_dump(data, sort_keys=False)
    _ensure_plugin_enabled(data)
    after = yaml.safe_dump(data, sort_keys=False)
    if before == after:
        return False
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_text(after)
    return True


def install_plugin(destination_root: Path, *, force: bool = False) -> Path:
    source = _repo_plugin_source()
    if not source.exists():
        raise FileNotFoundError(f"Plugin source not found at {source}")

    destination = destination_root / "plugins" / "hermes_iot"
    if destination.exists():
        if not force:
            raise FileExistsError(f"{destination} already exists. Use --force to replace it.")
        shutil.rmtree(destination)

    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination)
    _enable_plugin_config(destination_root)
    return destination


def _default_hermes_agent_root(hermes_home: Path) -> Path:
    return hermes_home / "hermes-agent"


def _replace_once(path: Path, needle: str, replacement: str) -> bool:
    text = path.read_text()
    if replacement in text:
        return False
    if needle not in text:
        raise RuntimeError(f"Could not find patch anchor in {path}: {needle!r}")
    path.write_text(text.replace(needle, replacement, 1))
    return True


def _install_platform_shim(hermes_agent_root: Path) -> Path:
    destination = hermes_agent_root / "gateway" / "platforms" / "iot.py"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(
        "from hermes_iot_gateway.hermes_adapter import IoTAdapter, check_iot_requirements\n"
    )
    return destination


def _install_python_path(hermes_agent_root: Path) -> Path | None:
    python = hermes_agent_root / "venv" / "bin" / "python"
    if not python.exists():
        return None
    output = subprocess.check_output(
        [
            str(python),
            "-c",
            "import site; print(site.getsitepackages()[0])",
        ],
        text=True,
    ).strip()
    site_packages = Path(output)
    site_packages.mkdir(parents=True, exist_ok=True)
    source_root = Path(__file__).resolve().parents[1]
    pth = site_packages / "hermes_iot_gateway_dev.pth"
    pth.write_text(f"{source_root}\n")
    return pth


def _install_runtime_dependencies(hermes_agent_root: Path) -> bool:
    python = hermes_agent_root / "venv" / "bin" / "python"
    uv = shutil.which("uv")
    if not python.exists() or uv is None:
        return False
    subprocess.run(
        [
            uv,
            "pip",
            "install",
            "--python",
            str(python),
            "aiortc>=1.9.0",
            "deepgram-sdk>=4.0.0",
            "pydantic-settings>=2.6.0",
            "websockets>=15.0.0",
        ],
        check=True,
    )
    return True


def _patch_gateway_config(hermes_agent_root: Path) -> bool:
    path = hermes_agent_root / "gateway" / "config.py"
    changed = False
    changed |= _replace_once(
        path,
        '    API_SERVER = "api_server"\n',
        '    API_SERVER = "api_server"\n    IOT = "iot"\n',
    )
    changed |= _replace_once(
        path,
        "            elif platform == Platform.API_SERVER:\n                connected.append(platform)\n",
        "            elif platform == Platform.API_SERVER:\n                connected.append(platform)\n"
        "            # Hermes IoT owns pairing/auth in the device transport layer.\n"
        "            elif platform == Platform.IOT:\n"
        "                connected.append(platform)\n",
    )
    return changed


def _patch_gateway_run(hermes_agent_root: Path) -> bool:
    path = hermes_agent_root / "gateway" / "run.py"
    changed = False
    needle = """        elif platform == Platform.API_SERVER:
            from gateway.platforms.api_server import APIServerAdapter, check_api_server_requirements
            if not check_api_server_requirements():
                logger.warning("API Server: aiohttp not installed")
                return None
            return APIServerAdapter(config)

"""
    replacement = needle + """        elif platform == Platform.IOT:
            from gateway.platforms.iot import IoTAdapter, check_iot_requirements
            if not check_iot_requirements():
                logger.warning("IoT: hermes-iot gateway package is not installed")
                return None
            return IoTAdapter(config)

"""
    changed |= _replace_once(path, needle, replacement)
    changed |= _replace_once(
        path,
        "                    enabled_toolsets=enabled_toolsets,\n"
        "                    ephemeral_system_prompt=combined_ephemeral or None,\n",
        "                    enabled_toolsets=enabled_toolsets,\n"
        "                    skip_memory=(source.platform == Platform.IOT),\n"
        "                    ephemeral_system_prompt=combined_ephemeral or None,\n",
    )
    changed |= _replace_once(
        path,
        "        if not history and source.platform and source.platform != Platform.LOCAL and source.platform != Platform.WEBHOOK:\n",
        "        if not history and source.platform and source.platform != Platform.LOCAL and source.platform not in {Platform.WEBHOOK, Platform.IOT}:\n",
    )
    changed |= _replace_once(
        path,
        '        platform_name = source.platform.value if source.platform else "unknown"\n'
        "        chat_id = source.chat_id\n",
        '        platform_name = source.platform.value if source.platform else "unknown"\n'
        "        if source.platform == Platform.IOT:\n"
        '            return "IoT voice devices cannot be set as a Hermes home channel."\n'
        "        chat_id = source.chat_id\n",
    )
    return changed


def _merge_profile_config(hermes_home: Path, *, host: str, port: int) -> bool:
    config_path = hermes_home / "config.yaml"
    data = yaml.safe_load(config_path.read_text()) if config_path.exists() else {}
    if not isinstance(data, dict):
        data = {}

    before = yaml.safe_dump(data, sort_keys=False)
    platforms = data.setdefault("platforms", {})
    if not isinstance(platforms, dict):
        platforms = {}
        data["platforms"] = platforms
    iot = platforms.setdefault("iot", {})
    if not isinstance(iot, dict):
        iot = {}
        platforms["iot"] = iot
    iot["enabled"] = True
    extra = iot.setdefault("extra", {})
    if not isinstance(extra, dict):
        extra = {}
        iot["extra"] = extra
    extra.setdefault("host", host)
    extra.setdefault("port", port)
    extra.setdefault("wake_word_enabled", False)

    toolsets = data.setdefault("platform_toolsets", {})
    if not isinstance(toolsets, dict):
        toolsets = {}
        data["platform_toolsets"] = toolsets
    toolsets["iot"] = ["hermes-iot", "no_mcp"]
    _ensure_plugin_enabled(data)

    after = yaml.safe_dump(data, sort_keys=False)
    if before == after:
        return False
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_text(after)
    return True


def install_native_gateway(
    hermes_home: Path,
    *,
    hermes_agent_root: Path | None = None,
    host: str = "0.0.0.0",
    port: int = 8787,
    update_profile: bool = True,
    install_dependencies: bool = True,
) -> dict[str, object]:
    root = hermes_agent_root or _default_hermes_agent_root(hermes_home)
    if not (root / "gateway" / "config.py").exists():
        raise FileNotFoundError(f"Hermes agent source not found at {root}")

    shim = _install_platform_shim(root)
    python_path = _install_python_path(root)
    dependencies = _install_runtime_dependencies(root) if install_dependencies else False
    changed = {
        "shim": str(shim),
        "python_path": str(python_path) if python_path else None,
        "dependencies": dependencies,
        "config_py": _patch_gateway_config(root),
        "run_py": _patch_gateway_run(root),
        "profile": _merge_profile_config(hermes_home, host=host, port=port) if update_profile else False,
    }
    return changed


def main() -> None:
    parser = argparse.ArgumentParser(description="Install Hermes IoT plugin assets into a Hermes profile")
    parser.add_argument("--hermes-home", default=str(default_hermes_home()))
    parser.add_argument("--hermes-agent-root", default=None)
    parser.add_argument("--native-gateway", action="store_true", help="Patch the local Hermes checkout with Platform.IOT")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--no-profile-update", action="store_true")
    parser.add_argument("--skip-dependencies", action="store_true")
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    hermes_home = Path(args.hermes_home)
    if args.native_gateway:
        result = install_native_gateway(
            hermes_home,
            hermes_agent_root=Path(args.hermes_agent_root) if args.hermes_agent_root else None,
            host=args.host,
            port=args.port,
            update_profile=not args.no_profile_update,
            install_dependencies=not args.skip_dependencies,
        )
        print(f"Installed Hermes IoT native gateway: {result}")
    else:
        destination = install_plugin(hermes_home, force=args.force)
        print(f"Installed Hermes IoT plugin to {destination}")
