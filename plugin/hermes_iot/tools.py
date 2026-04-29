from __future__ import annotations

import json
import os
import threading
import time
import uuid
from datetime import datetime
from zoneinfo import ZoneInfo

import httpx

_timers: dict[str, dict] = {}
_timers_lock = threading.Lock()
_cleanup_games: dict[str, dict] = {}
_cleanup_lock = threading.Lock()

_CLEANUP_CHALLENGES = [
    "Find five things that belong somewhere else.",
    "Rescue every shoe and jacket you can see.",
    "Clear the floor before the next chorus.",
    "Team mission: books, blocks, and dishes back home.",
    "Speed round: one surface completely clean.",
]


def _gateway_url() -> str:
    return os.environ.get("HERMES_IOT_GATEWAY_URL", "http://127.0.0.1:8787")


def _admin_key() -> str:
    return os.environ.get("HERMES_IOT_ADMIN_KEY", "dev-admin-key")


def _request(method: str, path: str, body: dict | None = None) -> dict:
    response = httpx.request(
        method,
        f"{_gateway_url()}{path}",
        headers={"X-Admin-Key": _admin_key()},
        json=body,
        timeout=10,
    )
    response.raise_for_status()
    return response.json()


def _default_device_id() -> str | None:
    return os.environ.get("HERMES_IOT_DEFAULT_DEVICE_ID")


def _safe_request(method: str, path: str, body: dict | None = None) -> dict:
    try:
        return _request(method, path, body)
    except Exception as exc:
        return {"error": str(exc)}


def _send_device_command(device_id: str | None, command: dict) -> dict:
    if not device_id:
        return {"accepted": False, "reason": "no device_id"}
    return _safe_request("POST", f"/v1/devices/{device_id}/commands", command)


def _notify_timer_done(timer_id: str) -> None:
    with _timers_lock:
        timer = _timers.get(timer_id)
        if timer is None:
            return
        timer["status"] = "done"
    device_id = timer.get("device_id")
    if not device_id:
        return
    label = timer.get("label") or "timer"
    for command in (
        {"type": "display_text", "payload": {"text": f"{label} timer done"}},
        {"type": "set_led", "payload": {"color": "amber", "pattern": "pulse"}},
        {"type": "beep", "payload": {"frequency_hz": 880, "duration_ms": 500}},
    ):
        try:
            _request("POST", f"/v1/devices/{device_id}/commands", command)
        except Exception:
            continue


def _cleanup_checkpoint(game_id: str, text: str, color: str = "amber") -> None:
    with _cleanup_lock:
        game = _cleanup_games.get(game_id)
        if game is None or game.get("status") != "running":
            return
        device_id = game.get("device_id")
    _send_device_command(device_id, {"type": "display_text", "payload": {"text": text}})
    _send_device_command(device_id, {"type": "set_led", "payload": {"color": color, "pattern": "pulse"}})
    _send_device_command(device_id, {"type": "beep", "payload": {"frequency_hz": 1046, "duration_ms": 120}})


def _cleanup_done(game_id: str) -> None:
    with _cleanup_lock:
        game = _cleanup_games.get(game_id)
        if game is None or game.get("status") != "running":
            return
        game["status"] = "done"
        device_id = game.get("device_id")
        game["ended_at"] = time.time()

    _safe_request("POST", "/v1/music/spotify/pause")
    _send_device_command(device_id, {"type": "display_text", "payload": {"text": "DONE"}})
    _send_device_command(device_id, {"type": "set_led", "payload": {"color": "green", "pattern": "pulse"}})
    for frequency in (784, 988, 1175):
        _send_device_command(device_id, {"type": "beep", "payload": {"frequency_hz": frequency, "duration_ms": 180}})


def _cleanup_start_media(game_id: str, music_query: str) -> None:
    with _cleanup_lock:
        game = _cleanup_games.get(game_id)
        if game is None or game.get("status") != "running":
            return
    media_start_result = _safe_request("POST", "/v1/music/librespot/start")
    with _cleanup_lock:
        game = _cleanup_games.get(game_id)
        if game is None or game.get("status") != "running":
            return
        game["media_start"] = media_start_result
    media_play_result = _safe_request("POST", "/v1/music/spotify/play", {"query": music_query})
    with _cleanup_lock:
        game = _cleanup_games.get(game_id)
        if game is not None:
            game["media_play"] = media_play_result


def _schedule_cleanup_timer(delay: float, callback, *args) -> threading.Timer:
    timer = threading.Timer(max(0.1, delay), callback, args=args)
    timer.daemon = True
    timer.start()
    return timer


def get_device_context(args, **kwargs):
    try:
        payload = _request("GET", f"/v1/devices/{args['device_id']}")
        return json.dumps(payload)
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def set_led(args, **kwargs):
    try:
        payload = _request(
            "POST",
            f"/v1/devices/{args['device_id']}/commands",
            {
                "type": "set_led",
                "payload": {
                    "color": args["color"],
                    "pattern": args.get("pattern", "solid"),
                },
            },
        )
        return json.dumps(payload)
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def beep(args, **kwargs):
    try:
        payload = _request(
            "POST",
            f"/v1/devices/{args['device_id']}/commands",
            {
                "type": "beep",
                "payload": {
                    "frequency_hz": args.get("frequency_hz", 880),
                    "duration_ms": args.get("duration_ms", 150),
                },
            },
        )
        return json.dumps(payload)
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def get_time(args, **kwargs):
    try:
        timezone = (args or {}).get("timezone")
        now = datetime.now(ZoneInfo(timezone)) if timezone else datetime.now().astimezone()
        return json.dumps(
            {
                "iso": now.isoformat(),
                "time": now.strftime("%-I:%M %p"),
                "date": now.strftime("%A, %B %-d, %Y"),
                "timezone": str(now.tzinfo),
            }
        )
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def set_timer(args, **kwargs):
    try:
        label = str(args.get("label") or "timer").strip()[:80] or "timer"
        duration_seconds = int(args["duration_seconds"])
        if duration_seconds < 1 or duration_seconds > 86400:
            raise ValueError("duration_seconds must be between 1 and 86400")
        device_id = args.get("device_id") or _default_device_id()
        timer_id = f"timer_{uuid.uuid4().hex[:12]}"
        ends_at = time.time() + duration_seconds
        timer = threading.Timer(duration_seconds, _notify_timer_done, args=(timer_id,))
        timer.daemon = True
        with _timers_lock:
            _timers[timer_id] = {
                "id": timer_id,
                "label": label,
                "duration_seconds": duration_seconds,
                "device_id": device_id,
                "ends_at": ends_at,
                "status": "running",
            }
        timer.start()
        return json.dumps(
            {
                "id": timer_id,
                "label": label,
                "duration_seconds": duration_seconds,
                "device_id": device_id,
                "ends_at": datetime.fromtimestamp(ends_at).astimezone().isoformat(),
                "status": "running",
            }
        )
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def cleanup_game(args, **kwargs):
    try:
        args = args or {}
        device_id = args.get("device_id") or _default_device_id()
        label = str(args.get("label") or "cleanup").strip()[:60] or "cleanup"
        if "duration_seconds" in args:
            duration_seconds = int(args["duration_seconds"])
        else:
            duration_minutes = int(args.get("duration_minutes") or 10)
            duration_seconds = duration_minutes * 60
        duration_seconds = max(10, min(duration_seconds, 30 * 60))
        music_query = str(args.get("music_query") or "clean up song kids").strip()[:120]
        challenge = _CLEANUP_CHALLENGES[int(time.time()) % len(_CLEANUP_CHALLENGES)]
        game_id = f"cleanup_{uuid.uuid4().hex[:10]}"
        ends_at = time.time() + duration_seconds

        _send_device_command(device_id, {"type": "display_text", "payload": {"text": "CLEANUP"}})
        _send_device_command(device_id, {"type": "set_led", "payload": {"color": "amber", "pattern": "cleanup"}})
        _send_device_command(device_id, {"type": "beep", "payload": {"frequency_hz": 880, "duration_ms": 140}})

        timers: list[threading.Timer] = []
        if duration_seconds >= 180:
            timers.append(_schedule_cleanup_timer(duration_seconds / 2, _cleanup_checkpoint, game_id, "HALFWAY", "blue"))
        if duration_seconds >= 90:
            timers.append(_schedule_cleanup_timer(duration_seconds - 60, _cleanup_checkpoint, game_id, "ONE MIN", "amber"))
        if duration_seconds >= 30:
            timers.append(_schedule_cleanup_timer(duration_seconds - 10, _cleanup_checkpoint, game_id, "FINAL", "red"))
        timers.append(_schedule_cleanup_timer(duration_seconds, _cleanup_done, game_id))

        with _cleanup_lock:
            _cleanup_games[game_id] = {
                "id": game_id,
                "label": label,
                "status": "running",
                "device_id": device_id,
                "duration_seconds": duration_seconds,
                "ends_at": ends_at,
                "music_query": music_query,
                "challenge": challenge,
                "timers": timers,
                "media_start": {"status": "scheduled"},
                "media_play": {"status": "scheduled"},
            }

        media_thread = threading.Thread(target=_cleanup_start_media, args=(game_id, music_query), daemon=True)
        media_thread.start()

        return json.dumps(
            {
                "id": game_id,
                "label": label,
                "status": "running",
                "device_id": device_id,
                "duration_seconds": duration_seconds,
                "ends_at": datetime.fromtimestamp(ends_at).astimezone().isoformat(),
                "music_query": music_query,
                "challenge": challenge,
                "media_start": {"status": "scheduled"},
                "media_play": {"status": "scheduled"},
            }
        )
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def cleanup_status(args, **kwargs):
    try:
        args = args or {}
        game_id = args.get("id")
        with _cleanup_lock:
            games = list(_cleanup_games.values())
            if game_id:
                games = [game for game in games if game.get("id") == game_id]
            summary = [
                {
                    "id": game["id"],
                    "label": game["label"],
                    "status": game["status"],
                    "device_id": game.get("device_id"),
                    "remaining_seconds": max(0, int(game["ends_at"] - time.time())),
                    "challenge": game.get("challenge"),
                }
                for game in games
            ]
        return json.dumps({"cleanup_games": summary})
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def cleanup_stop(args, **kwargs):
    try:
        args = args or {}
        game_id = args.get("id")
        stopped: list[str] = []
        with _cleanup_lock:
            games = [
                game
                for game in _cleanup_games.values()
                if game.get("status") == "running" and (not game_id or game.get("id") == game_id)
            ]
            for game in games:
                game["status"] = "stopped"
                game["ended_at"] = time.time()
                stopped.append(game["id"])
                for timer in game.get("timers", []):
                    timer.cancel()
                device_id = game.get("device_id")
                _send_device_command(device_id, {"type": "display_text", "payload": {"text": "STOPPED"}})
                _send_device_command(device_id, {"type": "set_led", "payload": {"color": "off", "pattern": "off"}})
        _safe_request("POST", "/v1/music/spotify/pause")
        return json.dumps({"stopped": stopped})
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def end_conversation(args, **kwargs):
    try:
        args = args or {}
        device_id = args.get("device_id") or _default_device_id()
        if not device_id:
            return json.dumps({"accepted": False, "reason": "no device_id"})
        return json.dumps(_request("POST", f"/v1/devices/{device_id}/end-conversation"))
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def media_status(args, **kwargs):
    try:
        return json.dumps(
            {
                "librespot": _request("GET", "/v1/music/librespot"),
                "spotify": _request("GET", "/v1/music/spotify/status"),
            }
        )
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def media_start(args, **kwargs):
    try:
        return json.dumps(_request("POST", "/v1/music/librespot/start"))
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def media_stop(args, **kwargs):
    try:
        return json.dumps(_request("POST", "/v1/music/librespot/stop"))
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def media_search(args, **kwargs):
    try:
        query = str(args["query"]).strip()
        media_type = str(args.get("media_type") or "track")
        limit = int(args.get("limit") or 5)
        response = httpx.request(
            "GET",
            f"{_gateway_url()}/v1/music/spotify/search",
            headers={"X-Admin-Key": _admin_key()},
            params={"q": query, "media_type": media_type, "limit": limit},
            timeout=10,
        )
        response.raise_for_status()
        return json.dumps(response.json())
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def media_play(args, **kwargs):
    try:
        body = {}
        if args.get("query"):
            body["query"] = args["query"]
        if args.get("uri"):
            body["uri"] = args["uri"]
        return json.dumps(_request("POST", "/v1/music/spotify/play", body))
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def media_pause(args, **kwargs):
    try:
        return json.dumps(_request("POST", "/v1/music/spotify/pause"))
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def media_resume(args, **kwargs):
    try:
        return json.dumps(_request("POST", "/v1/music/spotify/resume"))
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def media_next(args, **kwargs):
    try:
        return json.dumps(_request("POST", "/v1/music/spotify/next"))
    except Exception as exc:
        return json.dumps({"error": str(exc)})


def media_volume(args, **kwargs):
    try:
        return json.dumps(_request("POST", "/v1/music/spotify/volume", {"percent": int(args["percent"])}))
    except Exception as exc:
        return json.dumps({"error": str(exc)})
