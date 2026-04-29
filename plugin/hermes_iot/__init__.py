from __future__ import annotations

import shutil
from pathlib import Path

from . import schemas, tools


def _inject_device_context(**kwargs):
    platform = kwargs.get("platform")
    if platform not in {"api", "iot"}:
        return None
    return {
        "context": (
            "Hermes IoT session guidance:\n"
            "- You are speaking through a fixed home IoT voice device, not a desktop CLI or email client.\n"
            "- Prefer concise spoken replies.\n"
            "- For home-running tasks, use iot_get_time for current time and iot_set_timer for named timers.\n"
            "- If the user says it is time to clean up, start chores, clean the playroom, or similar, call iot_cleanup_game. Default to 10 minutes unless they specify a duration.\n"
            "- For cleanup status or stopping cleanup, use iot_cleanup_status or iot_cleanup_stop.\n"
            "- If the user ends the exchange with phrases like that's all, goodbye, thanks Willow, or we're done, briefly acknowledge and call iot_end_conversation.\n"
            "- For music, use only the iot_media_* tools. Do not use desktop automation or broad web/search tools to control Spotify.\n"
            "- Map music transport commands directly: skip/next song -> iot_media_next; pause/stop -> iot_media_pause or iot_media_stop; resume -> iot_media_resume. Do not call iot_media_play for skip, pause, stop, or resume.\n"
            "- Only call iot_media_play when the user asks to start a new song, album, artist, playlist, genre, or mood.\n"
            "- Do not suggest setting up desktop/email tools, local working directories, or generic Hermes skills unless the user explicitly asks for that maintenance flow.\n"
            "- Surface tool progress clearly when it changes the user-visible state.\n"
            "- Treat the attached device as interruptible and stateful.\n"
        )
    }


def _device_status_command(raw_args: str) -> str:
    device_id = raw_args.strip()
    if not device_id:
        return "Usage: /device <device-id>"
    return tools.get_device_context({"device_id": device_id})


def _install_skill() -> None:
    source = Path(__file__).parent / "skills" / "device-session" / "SKILL.md"
    if not source.exists():
        return
    try:
        from hermes_cli.config import get_hermes_home

        destination = get_hermes_home() / "skills" / "hermes-iot-device-session" / "SKILL.md"
    except Exception:
        destination = Path.home() / ".hermes" / "skills" / "hermes-iot-device-session" / "SKILL.md"
    if destination.exists():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def register(ctx):
    ctx.register_tool(
        name="iot_get_device_context",
        toolset="hermes-iot",
        schema=schemas.IOT_GET_DEVICE_CONTEXT,
        handler=tools.get_device_context,
    )
    ctx.register_tool(
        name="iot_set_led",
        toolset="hermes-iot",
        schema=schemas.IOT_SET_LED,
        handler=tools.set_led,
    )
    ctx.register_tool(
        name="iot_beep",
        toolset="hermes-iot",
        schema=schemas.IOT_BEEP,
        handler=tools.beep,
    )
    ctx.register_tool(
        name="iot_get_time",
        toolset="hermes-iot",
        schema=schemas.IOT_GET_TIME,
        handler=tools.get_time,
    )
    ctx.register_tool(
        name="iot_set_timer",
        toolset="hermes-iot",
        schema=schemas.IOT_SET_TIMER,
        handler=tools.set_timer,
    )
    ctx.register_tool(
        name="iot_cleanup_game",
        toolset="hermes-iot",
        schema=schemas.IOT_CLEANUP_GAME,
        handler=tools.cleanup_game,
    )
    ctx.register_tool(
        name="iot_cleanup_status",
        toolset="hermes-iot",
        schema=schemas.IOT_CLEANUP_STATUS,
        handler=tools.cleanup_status,
    )
    ctx.register_tool(
        name="iot_cleanup_stop",
        toolset="hermes-iot",
        schema=schemas.IOT_CLEANUP_STOP,
        handler=tools.cleanup_stop,
    )
    ctx.register_tool(
        name="iot_end_conversation",
        toolset="hermes-iot",
        schema=schemas.IOT_END_CONVERSATION,
        handler=tools.end_conversation,
    )
    ctx.register_tool(
        name="iot_media_status",
        toolset="hermes-iot",
        schema=schemas.IOT_MEDIA_STATUS,
        handler=tools.media_status,
    )
    ctx.register_tool(
        name="iot_media_start",
        toolset="hermes-iot",
        schema=schemas.IOT_MEDIA_START,
        handler=tools.media_start,
    )
    ctx.register_tool(
        name="iot_media_stop",
        toolset="hermes-iot",
        schema=schemas.IOT_MEDIA_STOP,
        handler=tools.media_stop,
    )
    ctx.register_tool(
        name="iot_media_search",
        toolset="hermes-iot",
        schema=schemas.IOT_MEDIA_SEARCH,
        handler=tools.media_search,
    )
    ctx.register_tool(
        name="iot_media_play",
        toolset="hermes-iot",
        schema=schemas.IOT_MEDIA_PLAY,
        handler=tools.media_play,
    )
    ctx.register_tool(
        name="iot_media_pause",
        toolset="hermes-iot",
        schema=schemas.IOT_MEDIA_PAUSE,
        handler=tools.media_pause,
    )
    ctx.register_tool(
        name="iot_media_resume",
        toolset="hermes-iot",
        schema=schemas.IOT_MEDIA_RESUME,
        handler=tools.media_resume,
    )
    ctx.register_tool(
        name="iot_media_next",
        toolset="hermes-iot",
        schema=schemas.IOT_MEDIA_NEXT,
        handler=tools.media_next,
    )
    ctx.register_tool(
        name="iot_media_volume",
        toolset="hermes-iot",
        schema=schemas.IOT_MEDIA_VOLUME,
        handler=tools.media_volume,
    )
    ctx.register_hook("pre_llm_call", _inject_device_context)
    ctx.register_command("device", handler=_device_status_command, description="Show Hermes IoT device status")
    _install_skill()
