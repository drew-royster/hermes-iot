import json
import time

from plugin.hermes_iot import tools


def test_get_time_returns_spoken_time_shape() -> None:
    payload = json.loads(tools.get_time({"timezone": "America/Denver"}))
    assert "time" in payload
    assert "date" in payload
    assert payload["iso"]


def test_set_timer_notifies_device_when_done(monkeypatch) -> None:
    calls: list[tuple[str, str, dict | None]] = []

    def fake_request(method: str, path: str, body: dict | None = None) -> dict:
        calls.append((method, path, body))
        return {"accepted": True}

    monkeypatch.setattr(tools, "_request", fake_request)
    payload = json.loads(
        tools.set_timer(
            {
                "label": "bread",
                "duration_seconds": 1,
                "device_id": "echo-test",
            }
        )
    )

    assert payload["label"] == "bread"
    assert payload["status"] == "running"
    time.sleep(1.2)
    assert len(calls) == 3
    assert all(call[0] == "POST" for call in calls)
    assert all(call[1] == "/v1/devices/echo-test/commands" for call in calls)
    assert [call[2]["type"] for call in calls if call[2]] == ["display_text", "set_led", "beep"]


def test_cleanup_game_starts_music_lights_and_can_stop(monkeypatch) -> None:
    calls: list[tuple[str, str, dict | None]] = []

    def fake_request(method: str, path: str, body: dict | None = None) -> dict:
        calls.append((method, path, body))
        return {"accepted": True}

    monkeypatch.setattr(tools, "_request", fake_request)
    tools._cleanup_games.clear()

    payload = json.loads(
        tools.cleanup_game(
            {
                "device_id": "echo-test",
                "duration_seconds": 10,
                "music_query": "cleanup jam",
            }
        )
    )

    assert payload["status"] == "running"
    assert payload["device_id"] == "echo-test"
    assert payload["duration_seconds"] == 10
    time.sleep(0.1)
    assert ("POST", "/v1/music/librespot/start", None) in calls
    assert ("POST", "/v1/music/spotify/play", {"query": "cleanup jam"}) in calls
    assert ("POST", "/v1/devices/echo-test/commands", {"type": "display_text", "payload": {"text": "CLEANUP"}}) in calls
    assert (
        "POST",
        "/v1/devices/echo-test/commands",
        {"type": "set_led", "payload": {"color": "amber", "pattern": "cleanup"}},
    ) in calls

    stopped = json.loads(tools.cleanup_stop({"id": payload["id"]}))
    assert stopped["stopped"] == [payload["id"]]
    assert ("POST", "/v1/music/spotify/pause", None) in calls


def test_end_conversation_uses_default_device(monkeypatch) -> None:
    calls: list[tuple[str, str, dict | None]] = []

    def fake_request(method: str, path: str, body: dict | None = None) -> dict:
        calls.append((method, path, body))
        return {"accepted": True, "status": "ending_after_turn", "device_id": "echo-test"}

    monkeypatch.setenv("HERMES_IOT_DEFAULT_DEVICE_ID", "echo-test")
    monkeypatch.setattr(tools, "_request", fake_request)

    payload = json.loads(tools.end_conversation({}))

    assert payload == {"accepted": True, "status": "ending_after_turn", "device_id": "echo-test"}
    assert calls == [("POST", "/v1/devices/echo-test/end-conversation", None)]
