import asyncio
import json

import httpx
import pytest

from hermes_iot_gateway.config import Settings
from hermes_iot_gateway.music import LibrespotService
from hermes_iot_gateway.registry import DeviceSession, InMemoryRegistry
from hermes_iot_gateway.spotify import SpotifyService


async def _librespot_pipe_requires_connected_output_track() -> None:
    registry = InMemoryRegistry()
    service = LibrespotService(
        Settings(
            HERMES_IOT_LIBRESPOT_BACKEND="pipe",
            HERMES_IOT_LIBRESPOT_TARGET_DEVICE_ID="echo-pyramid-dev",
        ),
        registry,
    )

    with pytest.raises(RuntimeError, match="target device is not connected"):
        await service.start()


def test_librespot_pipe_requires_connected_output_track() -> None:
    asyncio.run(_librespot_pipe_requires_connected_output_track())


class _FakeStdout:
    def __init__(self, chunks: list[bytes]) -> None:
        self._chunks = chunks

    async def read(self, _: int) -> bytes:
        if not self._chunks:
            return b""
        return self._chunks.pop(0)


class _FakeProcess:
    def __init__(self, chunks: list[bytes]) -> None:
        self.stdout = _FakeStdout(chunks)
        self.stderr = None
        self.returncode = None

    def terminate(self) -> None:
        self.returncode = -15


class _FakeOutputTrack:
    def __init__(self) -> None:
        self.chunks: list[bytes] = []

    async def push_pcm(self, chunk: bytes) -> None:
        self.chunks.append(chunk)


class _FakeSpeechRuntime:
    def __init__(self) -> None:
        self.paused: list[str] = []
        self.resumed: list[str] = []

    async def pause_audio_ingest(self, session) -> None:
        self.paused.append(session.device_id)

    async def resume_audio_ingest(self, session) -> None:
        self.resumed.append(session.device_id)


async def _librespot_pcm_pump_is_realtime_paced() -> None:
    registry = InMemoryRegistry()
    output = _FakeOutputTrack()
    session = DeviceSession(
        session_id="session-1",
        device_id="echo-pyramid-dev",
        conversation="iot:echo-pyramid-dev",
        output_track=output,
    )
    await registry.upsert_session(session)
    service = LibrespotService(
        Settings(
            HERMES_IOT_LIBRESPOT_BACKEND="pipe",
            HERMES_IOT_LIBRESPOT_OUTPUT_SAMPLE_RATE=100,
        ),
        registry,
    )
    service._ffmpeg_process = _FakeProcess([b"\x01\x00" * 2, b"\x02\x00" * 2, b"\x03\x00" * 2])

    started = asyncio.get_running_loop().time()
    await service._pump_pcm_to_device(session)
    elapsed = asyncio.get_running_loop().time() - started

    assert len(output.chunks) == 3
    assert elapsed >= 0.05


def test_librespot_pcm_pump_is_realtime_paced() -> None:
    asyncio.run(_librespot_pcm_pump_is_realtime_paced())


async def _librespot_pcm_pump_suppresses_media_during_wake_turn() -> None:
    registry = InMemoryRegistry()
    output = _FakeOutputTrack()
    session = DeviceSession(
        session_id="session-1",
        device_id="echo-pyramid-dev",
        conversation="iot:echo-pyramid-dev",
        assistant_state="listening",
        output_track=output,
        device_state={"media_playing": True, "wake_detected": True},
    )
    await registry.upsert_session(session)
    service = LibrespotService(
        Settings(
            HERMES_IOT_LIBRESPOT_BACKEND="pipe",
            HERMES_IOT_LIBRESPOT_OUTPUT_SAMPLE_RATE=100,
        ),
        registry,
    )
    service._ffmpeg_process = _FakeProcess([b"\x01\x00" * 2, b"\x02\x00" * 2])

    await service._pump_pcm_to_device(session)

    assert output.chunks == []


def test_librespot_pcm_pump_suppresses_media_during_wake_turn() -> None:
    asyncio.run(_librespot_pcm_pump_suppresses_media_during_wake_turn())


async def _librespot_media_mode_pauses_capture_and_notifies_device() -> None:
    registry = InMemoryRegistry()
    sent_messages: list[dict] = []

    async def _sender(message: dict) -> None:
        sent_messages.append(message)

    session = DeviceSession(
        session_id="session-1",
        device_id="echo-pyramid-dev",
        conversation="iot:echo-pyramid-dev",
        sender=_sender,
        capturing_audio=True,
        device_state={
            "wake_detected": True,
            "media_barge_in": True,
            "media_capture_reenabled_at": 123.0,
        },
    )
    await registry.upsert_session(session)
    speech = _FakeSpeechRuntime()
    service = LibrespotService(Settings(HERMES_IOT_LIBRESPOT_BACKEND="pipe"), registry, speech)

    await service._set_media_mode(session, playing=True)

    persisted = await registry.get_session_for_device("echo-pyramid-dev")
    assert persisted is not None
    assert persisted.capturing_audio is False
    assert persisted.device_state["media_playing"] is True
    assert "wake_detected" not in persisted.device_state
    assert "media_barge_in" not in persisted.device_state
    assert "media_capture_reenabled_at" not in persisted.device_state
    assert speech.paused == ["echo-pyramid-dev"]
    assert sent_messages == [
        {
            "type": "device.command",
            "payload": {"type": "media.mode", "playing": True},
            "timestamp": sent_messages[0]["timestamp"],
        }
    ]


def test_librespot_media_mode_pauses_capture_and_notifies_device() -> None:
    asyncio.run(_librespot_media_mode_pauses_capture_and_notifies_device())


async def _librespot_media_mode_resumes_capture_after_stop() -> None:
    registry = InMemoryRegistry()
    session = DeviceSession(
        session_id="session-1",
        device_id="echo-pyramid-dev",
        conversation="iot:echo-pyramid-dev",
        capturing_audio=False,
    )
    await registry.upsert_session(session)
    speech = _FakeSpeechRuntime()
    service = LibrespotService(Settings(HERMES_IOT_LIBRESPOT_BACKEND="pipe"), registry, speech)

    await service._set_media_mode(session, playing=False)

    persisted = await registry.get_session_for_device("echo-pyramid-dev")
    assert persisted is not None
    assert persisted.capturing_audio is True
    assert persisted.device_state["media_playing"] is False
    assert speech.resumed == ["echo-pyramid-dev"]


def test_librespot_media_mode_resumes_capture_after_stop() -> None:
    asyncio.run(_librespot_media_mode_resumes_capture_after_stop())


async def _spotify_search_simplifies_tracks(tmp_path) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path == "/v1/search":
            return httpx.Response(
                200,
                json={
                    "tracks": {
                        "items": [
                            {
                                "id": "track1",
                                "name": "Test Song",
                                "uri": "spotify:track:track1",
                                "type": "track",
                                "artists": [{"name": "Test Artist"}],
                                "album": {"name": "Test Album"},
                            }
                        ]
                    }
                },
            )
        return httpx.Response(404)

    token_path = tmp_path / "spotify.json"
    token_path.write_text(
        json.dumps({"access_token": "access", "refresh_token": "refresh", "expires_at": 9999999999}),
        encoding="utf-8",
    )
    service = SpotifyService(
        Settings(
            HERMES_IOT_SPOTIFY_CLIENT_ID="client",
            HERMES_IOT_SPOTIFY_TOKEN_PATH=str(token_path),
        ),
        httpx.AsyncClient(transport=httpx.MockTransport(handler)),
    )
    try:
        result = await service.search("test song")
    finally:
        await service.close()

    assert result["items"] == [
        {
            "name": "Test Song",
            "uri": "spotify:track:track1",
            "id": "track1",
            "type": "track",
            "artists": ["Test Artist"],
            "album": "Test Album",
        }
    ]


def test_spotify_search_simplifies_tracks(tmp_path) -> None:
    asyncio.run(_spotify_search_simplifies_tracks(tmp_path))


async def _spotify_search_skips_null_items(tmp_path) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.url.path == "/v1/search":
            return httpx.Response(
                200,
                json={
                    "playlists": {
                        "items": [
                            None,
                            {
                                "id": "playlist1",
                                "name": "Classical Focus",
                                "uri": "spotify:playlist:playlist1",
                                "type": "playlist",
                            },
                        ]
                    }
                },
            )
        return httpx.Response(404)

    token_path = tmp_path / "spotify.json"
    token_path.write_text(
        json.dumps({"access_token": "access", "refresh_token": "refresh", "expires_at": 9999999999}),
        encoding="utf-8",
    )
    service = SpotifyService(
        Settings(
            HERMES_IOT_SPOTIFY_CLIENT_ID="client",
            HERMES_IOT_SPOTIFY_TOKEN_PATH=str(token_path),
        ),
        httpx.AsyncClient(transport=httpx.MockTransport(handler)),
    )
    try:
        result = await service.search("classical music", media_type="playlist")
    finally:
        await service.close()

    assert result["items"] == [
        {
            "name": "Classical Focus",
            "uri": "spotify:playlist:playlist1",
            "id": "playlist1",
            "type": "playlist",
            "artists": [],
            "album": None,
        }
    ]


def test_spotify_search_skips_null_items(tmp_path) -> None:
    asyncio.run(_spotify_search_skips_null_items(tmp_path))


async def _spotify_play_searches_transfers_and_starts_track(tmp_path) -> None:
    calls: list[tuple[str, str, dict | None]] = []
    transferred = False

    def handler(request: httpx.Request) -> httpx.Response:
        nonlocal transferred
        body = json.loads(request.content.decode("utf-8")) if request.content else None
        calls.append((request.method, request.url.path, body))
        if request.url.path == "/v1/search":
            return httpx.Response(
                200,
                json={
                    "tracks": {
                        "items": [
                            {
                                "id": "track1",
                                "name": "Test Song",
                                "uri": "spotify:track:track1",
                                "type": "track",
                                "artists": [{"name": "Test Artist"}],
                                "album": {"name": "Test Album"},
                            }
                        ]
                    }
                },
            )
        if request.url.path == "/v1/me/player/devices":
            return httpx.Response(200, json={"devices": [{"id": "device1", "name": "Hermes Echo Pyramid"}]})
        if request.url.path == "/v1/me/player" and request.method == "GET":
            return httpx.Response(
                200,
                json={
                    "is_playing": transferred,
                    "device": {"id": "device1" if transferred else "other", "name": "Hermes Echo Pyramid"},
                    "item": {
                        "name": "Test Song",
                        "uri": "spotify:track:track1",
                    }
                    if transferred
                    else None,
                },
            )
        if request.url.path == "/v1/me/player":
            transferred = True
            return httpx.Response(204)
        if request.url.path == "/v1/me/player/play":
            transferred = True
            return httpx.Response(204)
        return httpx.Response(404)

    token_path = tmp_path / "spotify.json"
    token_path.write_text(
        json.dumps({"access_token": "access", "refresh_token": "refresh", "expires_at": 9999999999}),
        encoding="utf-8",
    )
    service = SpotifyService(
        Settings(
            HERMES_IOT_SPOTIFY_CLIENT_ID="client",
            HERMES_IOT_SPOTIFY_TOKEN_PATH=str(token_path),
        ),
        httpx.AsyncClient(transport=httpx.MockTransport(handler)),
    )
    try:
        result = await service.play(query="test song")
    finally:
        await service.close()

    assert result["accepted"] is True
    assert result["device_id"] == "device1"
    assert ("PUT", "/v1/me/player", {"device_ids": ["device1"], "play": False}) in calls
    assert ("PUT", "/v1/me/player/play", {"uris": ["spotify:track:track1"], "position_ms": 0}) in calls


def test_spotify_play_searches_transfers_and_starts_track(tmp_path) -> None:
    asyncio.run(_spotify_play_searches_transfers_and_starts_track(tmp_path))


async def _spotify_play_accepts_context_playback(tmp_path) -> None:
    calls: list[tuple[str, str, dict | None]] = []
    transferred = False

    def handler(request: httpx.Request) -> httpx.Response:
        nonlocal transferred
        body = json.loads(request.content.decode("utf-8")) if request.content else None
        calls.append((request.method, request.url.path, body))
        if request.url.path == "/v1/me/player/devices":
            return httpx.Response(200, json={"devices": [{"id": "device1", "name": "Hermes Echo Pyramid"}]})
        if request.url.path == "/v1/me/player" and request.method == "GET":
            return httpx.Response(
                200,
                json={
                    "is_playing": transferred,
                    "device": {"id": "device1", "name": "Hermes Echo Pyramid"},
                    "context": {"uri": "spotify:playlist:playlist1"} if transferred else None,
                    "item": {"name": "Playlist Track", "uri": "spotify:track:track1"} if transferred else None,
                },
            )
        if request.url.path == "/v1/me/player/play":
            transferred = True
            return httpx.Response(204)
        return httpx.Response(404)

    token_path = tmp_path / "spotify.json"
    token_path.write_text(
        json.dumps({"access_token": "access", "refresh_token": "refresh", "expires_at": 9999999999}),
        encoding="utf-8",
    )
    service = SpotifyService(
        Settings(
            HERMES_IOT_SPOTIFY_CLIENT_ID="client",
            HERMES_IOT_SPOTIFY_TOKEN_PATH=str(token_path),
        ),
        httpx.AsyncClient(transport=httpx.MockTransport(handler)),
    )
    try:
        result = await service.play(uri="spotify:playlist:playlist1")
    finally:
        await service.close()

    assert result["accepted"] is True
    assert result["playback"]["context_uri"] == "spotify:playlist:playlist1"
    assert ("PUT", "/v1/me/player/play", {"context_uri": "spotify:playlist:playlist1", "position_ms": 0}) in calls


def test_spotify_play_accepts_context_playback(tmp_path) -> None:
    asyncio.run(_spotify_play_accepts_context_playback(tmp_path))
