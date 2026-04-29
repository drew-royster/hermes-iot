from __future__ import annotations

import base64
import hashlib
import asyncio
import json
import logging
import secrets
import time
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

import httpx

from .config import Settings


SPOTIFY_SCOPES = [
    "user-read-playback-state",
    "user-read-currently-playing",
    "user-modify-playback-state",
]

logger = logging.getLogger(__name__)


class SpotifyService:
    def __init__(self, settings: Settings, http_client: httpx.AsyncClient | None = None) -> None:
        self._settings = settings
        self._client = http_client or httpx.AsyncClient(timeout=20)
        self._owns_client = http_client is None
        self._state: str | None = None
        self._code_verifier: str | None = None

    async def close(self) -> None:
        if self._owns_client:
            await self._client.aclose()

    def status(self) -> dict[str, Any]:
        token = self._load_token()
        return {
            "configured": bool(self._settings.spotify_client_id),
            "authenticated": bool(token and token.get("refresh_token")),
            "expires_at": token.get("expires_at") if token else None,
            "device_name": self._settings.spotify_device_name,
            "device_id": self._settings.spotify_device_id,
            "token_path": str(Path(self._settings.spotify_token_path).expanduser()),
            "scopes": SPOTIFY_SCOPES,
        }

    def build_login_url(self) -> dict[str, str]:
        if not self._settings.spotify_client_id:
            raise RuntimeError("HERMES_IOT_SPOTIFY_CLIENT_ID is required")
        self._state = secrets.token_urlsafe(24)
        self._code_verifier = secrets.token_urlsafe(64)
        digest = hashlib.sha256(self._code_verifier.encode("ascii")).digest()
        challenge = base64.urlsafe_b64encode(digest).decode("ascii").rstrip("=")
        params = {
            "response_type": "code",
            "client_id": self._settings.spotify_client_id,
            "scope": " ".join(SPOTIFY_SCOPES),
            "redirect_uri": self._settings.spotify_redirect_uri,
            "state": self._state,
            "code_challenge_method": "S256",
            "code_challenge": challenge,
        }
        return {"auth_url": f"https://accounts.spotify.com/authorize?{urlencode(params)}"}

    async def handle_callback(self, *, code: str, state: str) -> dict[str, Any]:
        if not self._settings.spotify_client_id:
            raise RuntimeError("HERMES_IOT_SPOTIFY_CLIENT_ID is required")
        if not self._state or not self._code_verifier or state != self._state:
            raise RuntimeError("Invalid Spotify OAuth state")
        payload = {
            "grant_type": "authorization_code",
            "code": code,
            "redirect_uri": self._settings.spotify_redirect_uri,
            "client_id": self._settings.spotify_client_id,
            "code_verifier": self._code_verifier,
        }
        token = await self._token_request(payload)
        self._save_token(token)
        self._state = None
        self._code_verifier = None
        return self.status()

    async def search(self, query: str, *, media_type: str = "track", limit: int = 5) -> dict[str, Any]:
        media_type = media_type if media_type in {"track", "album", "playlist"} else "track"
        limit = max(1, min(limit, 10))
        payload = await self._api_request(
            "GET",
            "/v1/search",
            params={"q": query, "type": media_type, "limit": limit},
        )
        return {"items": self._simplify_search(payload, media_type)}

    async def devices(self) -> dict[str, Any]:
        payload = await self._api_request("GET", "/v1/me/player/devices")
        devices = [
            {
                "id": device.get("id"),
                "name": device.get("name"),
                "type": device.get("type"),
                "is_active": device.get("is_active"),
                "volume_percent": device.get("volume_percent"),
            }
            for device in payload.get("devices", [])
        ]
        return {"devices": devices}

    async def play(self, *, query: str | None = None, uri: str | None = None, device_id: str | None = None) -> dict[str, Any]:
        target_uri = uri
        selected: dict[str, Any] | None = None
        if not target_uri:
            if not query:
                raise RuntimeError("query or uri is required")
            result = await self.search(query, media_type="track", limit=1)
            items = result["items"]
            if not items:
                raise RuntimeError(f"No Spotify track found for: {query}")
            selected = items[0]
            target_uri = selected["uri"]

        target_device_id = device_id or await self._resolve_device_id(wait_seconds=8)
        logger.info("Spotify play requested: query=%r uri=%r device_id=%s", query, target_uri, target_device_id)
        await self._ensure_active_device(target_device_id)
        await self._play_uri(target_uri, target_device_id)

        playback = await self._wait_for_playback(target_uri, target_device_id)
        return {
            "accepted": True,
            "device_id": target_device_id,
            "uri": target_uri,
            "item": selected,
            "playback": playback,
        }

    async def pause(self) -> dict[str, bool]:
        await self._api_request("PUT", "/v1/me/player/pause", expect_json=False)
        return {"accepted": True}

    async def resume(self) -> dict[str, bool]:
        await self._api_request("PUT", "/v1/me/player/play", expect_json=False)
        return {"accepted": True}

    async def next(self) -> dict[str, bool]:
        logger.info("Spotify next requested")
        await self._api_request("POST", "/v1/me/player/next", expect_json=False)
        return {"accepted": True}

    async def volume(self, percent: int) -> dict[str, Any]:
        await self._api_request(
            "PUT",
            "/v1/me/player/volume",
            params={"volume_percent": max(0, min(percent, 100))},
            expect_json=False,
        )
        return {"accepted": True, "volume_percent": percent}

    async def playback(self) -> dict[str, Any]:
        return await self._api_request("GET", "/v1/me/player")

    async def _resolve_device_id(self, *, wait_seconds: float = 0.0) -> str:
        if self._settings.spotify_device_id:
            return self._settings.spotify_device_id
        deadline = time.monotonic() + wait_seconds
        while True:
            payload = await self.devices()
            for device in payload["devices"]:
                if device.get("name") == self._settings.spotify_device_name:
                    return str(device["id"])
            if time.monotonic() >= deadline:
                break
            await asyncio.sleep(0.5)
        raise RuntimeError(f"Spotify Connect device not found: {self._settings.spotify_device_name}")

    async def _ensure_active_device(self, device_id: str) -> None:
        playback = await self._api_request("GET", "/v1/me/player")
        current = playback.get("device") or {}
        if current.get("id") == device_id:
            return
        await self._api_request(
            "PUT",
            "/v1/me/player",
            json={"device_ids": [device_id], "play": False},
            expect_json=False,
        )
        for _ in range(16):
            await asyncio.sleep(0.25)
            playback = await self._api_request("GET", "/v1/me/player")
            current = playback.get("device") or {}
            if current.get("id") == device_id:
                return
        raise RuntimeError(f"Spotify did not transfer playback to device: {device_id}")

    async def _play_uri(self, uri: str, device_id: str) -> None:
        body = {"uris": [uri], "position_ms": 0} if uri.startswith("spotify:track:") else {"context_uri": uri, "position_ms": 0}
        await self._api_request(
            "PUT",
            "/v1/me/player/play",
            params={"device_id": device_id},
            json=body,
            expect_json=False,
        )

    async def _wait_for_playback(self, uri: str, device_id: str) -> dict[str, Any]:
        last: dict[str, Any] = {}
        is_track_uri = uri.startswith("spotify:track:")
        for attempt in range(20):
            await asyncio.sleep(0.25)
            last = await self._api_request("GET", "/v1/me/player")
            current_device = last.get("device") or {}
            item = last.get("item") or {}
            context = last.get("context") or {}
            device_matches = current_device.get("id") == device_id
            is_playing = bool(last.get("is_playing"))
            track_matches = item.get("uri") == uri
            context_matches = context.get("uri") == uri
            if device_matches and is_playing and ((is_track_uri and track_matches) or (not is_track_uri and context_matches)):
                return {
                    "is_playing": True,
                    "device": current_device.get("name"),
                    "track": item.get("name"),
                    "uri": item.get("uri"),
                    "context_uri": context.get("uri"),
                }
            if attempt == 7:
                await self._play_uri(uri, device_id)
        item = last.get("item") or {}
        context = last.get("context") or {}
        raise RuntimeError(
            "Spotify did not settle on requested playback; "
            f"is_playing={last.get('is_playing')} current_uri={item.get('uri')} "
            f"context_uri={context.get('uri')} requested_uri={uri}"
        )

    async def _api_request(
        self,
        method: str,
        path: str,
        *,
        params: dict[str, Any] | None = None,
        json: dict[str, Any] | None = None,
        expect_json: bool = True,
    ) -> dict[str, Any]:
        token = await self._access_token()
        response = await self._client.request(
            method,
            f"https://api.spotify.com{path}",
            headers={"Authorization": f"Bearer {token}"},
            params=params,
            json=json,
        )
        if response.status_code == 401:
            token = await self._refresh_token()
            response = await self._client.request(
                method,
                f"https://api.spotify.com{path}",
                headers={"Authorization": f"Bearer {token}"},
                params=params,
                json=json,
            )
        if response.status_code == 204 or not expect_json:
            if response.is_error:
                raise RuntimeError(self._error_message(response))
            return {}
        if response.is_error:
            raise RuntimeError(self._error_message(response))
        return response.json()

    async def _access_token(self) -> str:
        token = self._load_token()
        if not token or not token.get("refresh_token"):
            raise RuntimeError("Spotify is not authenticated")
        if float(token.get("expires_at", 0)) <= time.time() + 60:
            return await self._refresh_token()
        return str(token["access_token"])

    async def _refresh_token(self) -> str:
        token = self._load_token()
        if not token or not token.get("refresh_token"):
            raise RuntimeError("Spotify is not authenticated")
        payload = {
            "grant_type": "refresh_token",
            "refresh_token": token["refresh_token"],
            "client_id": self._settings.spotify_client_id,
        }
        refreshed = await self._token_request(payload)
        if not refreshed.get("refresh_token"):
            refreshed["refresh_token"] = token["refresh_token"]
        self._save_token(refreshed)
        return str(refreshed["access_token"])

    async def _token_request(self, payload: dict[str, Any]) -> dict[str, Any]:
        response = await self._client.post("https://accounts.spotify.com/api/token", data=payload)
        if response.is_error:
            raise RuntimeError(self._error_message(response))
        token = response.json()
        token["expires_at"] = time.time() + int(token.get("expires_in", 3600))
        return token

    def _load_token(self) -> dict[str, Any] | None:
        path = Path(self._settings.spotify_token_path).expanduser()
        if not path.exists():
            return None
        return json.loads(path.read_text(encoding="utf-8"))

    def _save_token(self, token: dict[str, Any]) -> None:
        path = Path(self._settings.spotify_token_path).expanduser()
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(token, indent=2, sort_keys=True), encoding="utf-8")

    @staticmethod
    def _simplify_search(payload: dict[str, Any], media_type: str) -> list[dict[str, Any]]:
        container = payload.get(f"{media_type}s", {})
        items = container.get("items", [])
        simplified = []
        for item in items:
            if not isinstance(item, dict):
                continue
            artists = item.get("artists") or []
            simplified.append(
                {
                    "name": item.get("name"),
                    "uri": item.get("uri"),
                    "id": item.get("id"),
                    "type": item.get("type"),
                    "artists": [artist.get("name") for artist in artists if artist.get("name")],
                    "album": (item.get("album") or {}).get("name"),
                }
            )
        return simplified

    @staticmethod
    def _error_message(response: httpx.Response) -> str:
        try:
            payload = response.json()
        except ValueError:
            return response.text or f"Spotify API failed with status {response.status_code}"
        error = payload.get("error", payload)
        if isinstance(error, dict):
            return error.get("message") or json.dumps(error)
        return str(error)
