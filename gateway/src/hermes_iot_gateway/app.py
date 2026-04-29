from __future__ import annotations

import argparse
import logging

import uvicorn

from .config import resolve_settings
from .runtime import create_app, create_runtime


def main() -> None:
    parser = argparse.ArgumentParser(description="Run the Hermes IoT WebRTC gateway")
    parser.add_argument("--hermes-home", default=None, help="Hermes profile directory to read config from")
    args = parser.parse_args()

    logging.basicConfig(level=logging.INFO)
    settings = resolve_settings(hermes_home=args.hermes_home)
    runtime = create_runtime(settings)
    app = create_app(runtime)
    uvicorn.run(app, host=settings.host, port=settings.port)
