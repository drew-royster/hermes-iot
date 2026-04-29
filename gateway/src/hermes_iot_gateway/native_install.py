from __future__ import annotations

import argparse
from pathlib import Path

from .installer import install_native_gateway
from .profile import default_hermes_home


def main() -> None:
    parser = argparse.ArgumentParser(description="Install Hermes IoT as a native Hermes gateway platform")
    parser.add_argument("--hermes-home", default=str(default_hermes_home()))
    parser.add_argument("--hermes-agent-root", default=None)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--no-profile-update", action="store_true")
    parser.add_argument("--skip-dependencies", action="store_true")
    args = parser.parse_args()

    hermes_home = Path(args.hermes_home)
    result = install_native_gateway(
        hermes_home,
        hermes_agent_root=Path(args.hermes_agent_root) if args.hermes_agent_root else None,
        host=args.host,
        port=args.port,
        update_profile=not args.no_profile_update,
        install_dependencies=not args.skip_dependencies,
    )
    print(f"Installed Hermes IoT native gateway: {result}")
