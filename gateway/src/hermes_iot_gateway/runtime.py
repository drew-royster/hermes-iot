from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path

import httpx
from fastapi import Depends, FastAPI, Header, HTTPException, Request, status
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from .config import Settings, resolve_settings
from .deepgram import (
    DeepgramAuraConfig,
    DeepgramAuraTextToSpeechProvider,
    DeepgramFluxConfig,
    DeepgramFluxSpeechToTextProvider,
)
from .hermes import HermesResponsesClient
from .models import (
    DebugPlaybackRequest,
    DeviceCommandRequest,
    DeviceSummary,
    IceCandidateRequest,
    InterruptRequest,
    PairClaimRequest,
    PairClaimResponse,
    SpotifyPlayRequest,
    SpotifyVolumeRequest,
    WebRTCOfferRequest,
    WebRTCOfferResponse,
)
from .music import LibrespotService
from .registry import PersistentRegistry
from .session import GatewaySessionManager, NativeGatewaySessionManager, NativeTurnHandler
from .speech import DebugSpeechToTextProvider, NoOpTextToSpeechProvider, SpeechRuntime
from .spotify import SpotifyService
from .storage import SQLiteStateStore
from .webrtc import WebRTCService

logger = logging.getLogger(__name__)


@dataclass(slots=True)
class GatewayRuntime:
    settings: Settings
    store: SQLiteStateStore
    registry: PersistentRegistry
    hermes: HermesResponsesClient | None
    speech: SpeechRuntime
    sessions: GatewaySessionManager
    webrtc: WebRTCService
    librespot: LibrespotService
    spotify: SpotifyService

    async def initialize(self) -> None:
        await self.registry.initialize()
        if self.settings.librespot_enabled:
            await self.librespot.start()

    async def close(self) -> None:
        await self.librespot.close()
        await self.spotify.close()
        if self.hermes is not None:
            await self.hermes._client.aclose()


def create_runtime(
    settings: Settings | None = None,
    *,
    hermes_http_client: httpx.AsyncClient | None = None,
) -> GatewayRuntime:
    resolved = resolve_settings(settings)
    store = SQLiteStateStore(resolved.state_db_path)
    registry = PersistentRegistry(store)
    hermes = HermesResponsesClient(resolved, http_client=hermes_http_client)
    if resolved.speech_to_text_provider == "deepgram" and resolved.deepgram_api_key:
        stt_provider = DeepgramFluxSpeechToTextProvider(
            DeepgramFluxConfig(
                api_key=resolved.deepgram_api_key,
                model=resolved.deepgram_flux_model,
                sample_rate=resolved.deepgram_flux_sample_rate,
                eot_threshold=resolved.deepgram_flux_eot_threshold,
                eager_eot_threshold=resolved.deepgram_flux_eager_eot_threshold,
                eot_timeout_ms=resolved.deepgram_flux_eot_timeout_ms,
            )
        )
    else:
        stt_provider = DebugSpeechToTextProvider()

    if resolved.text_to_speech_provider == "deepgram" and resolved.deepgram_api_key:
        tts_provider = DeepgramAuraTextToSpeechProvider(
            DeepgramAuraConfig(
                api_key=resolved.deepgram_api_key,
                model=resolved.deepgram_aura_model,
                sample_rate=resolved.deepgram_aura_sample_rate,
            )
        )
    else:
        tts_provider = NoOpTextToSpeechProvider()

    speech = SpeechRuntime(stt_provider, tts_provider)
    spotify = SpotifyService(resolved)

    async def sync_spotify_volume(session, volume: int) -> None:
        if not session.device_state.get("media_playing"):
            return
        try:
            await spotify.volume(volume)
        except RuntimeError as exc:
            logger.warning("Failed to sync device volume to Spotify for %s: %s", session.device_id, exc)

    sessions = GatewaySessionManager(
        registry,
        hermes,
        speech,
        conversation_mode=resolved.conversation_mode,
        wake_word_enabled=resolved.wake_word_enabled,
        wake_word=resolved.wake_word,
        sleep_timeout_seconds=resolved.sleep_timeout_seconds,
        on_device_volume=sync_spotify_volume,
    )
    webrtc = WebRTCService(resolved, registry, sessions)
    librespot = LibrespotService(resolved, registry, speech)
    return GatewayRuntime(
        settings=resolved,
        store=store,
        registry=registry,
        hermes=hermes,
        speech=speech,
        sessions=sessions,
        webrtc=webrtc,
        librespot=librespot,
        spotify=spotify,
    )


def create_native_runtime(
    handle_turn: NativeTurnHandler,
    settings: Settings | None = None,
) -> GatewayRuntime:
    resolved = resolve_settings(settings)
    store = SQLiteStateStore(resolved.state_db_path)
    registry = PersistentRegistry(store)
    if resolved.speech_to_text_provider == "deepgram" and resolved.deepgram_api_key:
        stt_provider = DeepgramFluxSpeechToTextProvider(
            DeepgramFluxConfig(
                api_key=resolved.deepgram_api_key,
                model=resolved.deepgram_flux_model,
                sample_rate=resolved.deepgram_flux_sample_rate,
                eot_threshold=resolved.deepgram_flux_eot_threshold,
                eager_eot_threshold=resolved.deepgram_flux_eager_eot_threshold,
                eot_timeout_ms=resolved.deepgram_flux_eot_timeout_ms,
            )
        )
    else:
        stt_provider = DebugSpeechToTextProvider()

    if resolved.text_to_speech_provider == "deepgram" and resolved.deepgram_api_key:
        tts_provider = DeepgramAuraTextToSpeechProvider(
            DeepgramAuraConfig(
                api_key=resolved.deepgram_api_key,
                model=resolved.deepgram_aura_model,
                sample_rate=resolved.deepgram_aura_sample_rate,
            )
        )
    else:
        tts_provider = NoOpTextToSpeechProvider()

    speech = SpeechRuntime(stt_provider, tts_provider)
    spotify = SpotifyService(resolved)

    async def sync_spotify_volume(session, volume: int) -> None:
        if not session.device_state.get("media_playing"):
            return
        try:
            await spotify.volume(volume)
        except RuntimeError as exc:
            logger.warning("Failed to sync device volume to Spotify for %s: %s", session.device_id, exc)

    sessions = NativeGatewaySessionManager(
        registry,
        speech,
        handle_turn,
        conversation_mode=resolved.conversation_mode,
        wake_word_enabled=resolved.wake_word_enabled,
        wake_word=resolved.wake_word,
        sleep_timeout_seconds=resolved.sleep_timeout_seconds,
        on_device_volume=sync_spotify_volume,
    )
    webrtc = WebRTCService(resolved, registry, sessions)
    librespot = LibrespotService(resolved, registry, speech)
    return GatewayRuntime(
        settings=resolved,
        store=store,
        registry=registry,
        hermes=None,
        speech=speech,
        sessions=sessions,
        webrtc=webrtc,
        librespot=librespot,
        spotify=spotify,
    )


def create_app(runtime: GatewayRuntime | None = None) -> FastAPI:
    resolved_runtime = runtime or create_runtime()
    web_dir = Path(__file__).with_name("web")

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        await resolved_runtime.initialize()
        yield
        await resolved_runtime.close()

    app = FastAPI(title="Hermes IoT Gateway", version="0.1.0", lifespan=lifespan)
    app.mount("/debug/webrtc/assets", StaticFiles(directory=web_dir), name="debug-webrtc-assets")

    async def require_device(authorization: str = Header(default="")):
        if not authorization.startswith("Bearer "):
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Missing bearer token")
        token = authorization.split(" ", 1)[1]
        record = await resolved_runtime.registry.get_device_by_token(token)
        if record is None:
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid device token")
        return record

    async def require_admin(x_admin_key: str = Header(default="")):
        if x_admin_key != resolved_runtime.settings.admin_key:
            raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Invalid admin key")

    @app.get("/health")
    async def health() -> dict[str, str]:
        return {"status": "ok"}

    @app.get("/debug/webrtc", include_in_schema=False)
    async def debug_webrtc_ui() -> FileResponse:
        return FileResponse(web_dir / "index.html")

    @app.post("/v1/pair/claim", response_model=PairClaimResponse)
    async def pair_claim(request: PairClaimRequest, http_request: Request) -> PairClaimResponse:
        # TODO: Restore an explicit pairing secret or admin-approved enrollment flow
        # before shipping beyond trusted local development.
        record = await resolved_runtime.registry.claim_device(
            device_id=request.device_id,
            firmware_version=request.firmware_version,
            capabilities=request.capabilities,
        )
        return PairClaimResponse(
            device_id=record.device_id,
            auth_token=record.auth_token,
            signaling_url=str(http_request.url_for("accept_offer", device_id=record.device_id)),
            expires_at=record.last_seen_at,
            conversation=record.conversation,
        )

    @app.post("/v1/devices/{device_id}/webrtc/offer", response_model=WebRTCOfferResponse)
    async def accept_offer(
        device_id: str,
        request: WebRTCOfferRequest,
        device=Depends(require_device),
    ) -> WebRTCOfferResponse:
        if device.device_id != device_id or request.hello.device_id != device_id:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="Device ID mismatch")
        return await resolved_runtime.webrtc.accept_offer(device_id, request)

    @app.post("/v1/devices/{device_id}/webrtc/ice")
    async def add_ice_candidate(
        device_id: str,
        request: IceCandidateRequest,
        device=Depends(require_device),
    ) -> dict[str, bool]:
        if device.device_id != device_id:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="Device ID mismatch")
        added = await resolved_runtime.webrtc.add_ice_candidate(device_id, request)
        return {"accepted": added}

    @app.post("/v1/devices/{device_id}/interrupt")
    async def interrupt(
        device_id: str,
        request: InterruptRequest,
        device=Depends(require_device),
    ) -> dict[str, bool]:
        if device.device_id != device_id:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail="Device ID mismatch")
        interrupted = await resolved_runtime.sessions.interrupt(device_id, request.reason)
        return {"interrupted": interrupted}

    @app.post("/v1/devices/{device_id}/end-conversation", dependencies=[Depends(require_admin)])
    async def end_conversation(device_id: str) -> dict[str, object]:
        result = await resolved_runtime.sessions.end_conversation(device_id, reason="tool")
        if not result.get("accepted"):
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=result)
        return result

    @app.get("/v1/devices", response_model=list[DeviceSummary], dependencies=[Depends(require_admin)])
    async def list_devices() -> list[DeviceSummary]:
        summaries: list[DeviceSummary] = []
        for record, session in await resolved_runtime.registry.list_devices():
            summaries.append(
                DeviceSummary(
                    device_id=record.device_id,
                    paired_at=record.paired_at,
                    last_seen_at=record.last_seen_at,
                    firmware_version=record.firmware_version,
                    capabilities=record.capabilities,
                    connected=bool(session and session.connected),
                    conversation=record.conversation,
                    session_id=session.session_id if session else None,
                    device_state=session.device_state if session else {},
                )
            )
        return summaries

    @app.get("/v1/devices/{device_id}", response_model=DeviceSummary, dependencies=[Depends(require_admin)])
    async def get_device(device_id: str) -> DeviceSummary:
        record = await resolved_runtime.registry.get_device(device_id)
        if record is None:
            raise HTTPException(status_code=status.HTTP_404_NOT_FOUND, detail="Unknown device")
        session = await resolved_runtime.registry.get_session_for_device(device_id)
        return DeviceSummary(
            device_id=record.device_id,
            paired_at=record.paired_at,
            last_seen_at=record.last_seen_at,
            firmware_version=record.firmware_version,
            capabilities=record.capabilities,
            connected=bool(session and session.connected),
            conversation=record.conversation,
            session_id=session.session_id if session else None,
            device_state=session.device_state if session else {},
        )

    @app.post("/v1/devices/{device_id}/commands", dependencies=[Depends(require_admin)])
    async def send_command(device_id: str, request: DeviceCommandRequest) -> dict[str, bool]:
        accepted = await resolved_runtime.sessions.send_command(device_id, request)
        if not accepted:
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail="Device is not connected")
        return {"accepted": True}

    @app.post("/debug/devices/{device_id}/playback", dependencies=[Depends(require_admin)])
    async def debug_playback(device_id: str, request: DebugPlaybackRequest) -> dict[str, object]:
        try:
            result = await resolved_runtime.sessions.debug_playback(device_id, request)
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc
        if not result.get("accepted"):
            raise HTTPException(status_code=status.HTTP_409_CONFLICT, detail=result)
        return result

    @app.get("/v1/music/librespot", dependencies=[Depends(require_admin)])
    async def librespot_status() -> dict[str, object]:
        return resolved_runtime.librespot.status()

    @app.post("/v1/music/librespot/start", dependencies=[Depends(require_admin)])
    async def start_librespot() -> dict[str, object]:
        try:
            return await resolved_runtime.librespot.start()
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.post("/v1/music/librespot/stop", dependencies=[Depends(require_admin)])
    async def stop_librespot() -> dict[str, object]:
        return await resolved_runtime.librespot.stop()

    @app.get("/v1/music/spotify/status", dependencies=[Depends(require_admin)])
    async def spotify_status() -> dict[str, object]:
        return resolved_runtime.spotify.status()

    @app.get("/v1/music/spotify/login", dependencies=[Depends(require_admin)])
    async def spotify_login() -> dict[str, str]:
        try:
            return resolved_runtime.spotify.build_login_url()
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.get("/v1/music/spotify/callback")
    async def spotify_callback(code: str, state: str) -> dict[str, object]:
        try:
            return await resolved_runtime.spotify.handle_callback(code=code, state=state)
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.get("/v1/music/spotify/search", dependencies=[Depends(require_admin)])
    async def spotify_search(q: str, media_type: str = "track", limit: int = 5) -> dict[str, object]:
        try:
            return await resolved_runtime.spotify.search(q, media_type=media_type, limit=limit)
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.get("/v1/music/spotify/devices", dependencies=[Depends(require_admin)])
    async def spotify_devices() -> dict[str, object]:
        try:
            return await resolved_runtime.spotify.devices()
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.post("/v1/music/spotify/play", dependencies=[Depends(require_admin)])
    async def spotify_play(request: SpotifyPlayRequest) -> dict[str, object]:
        try:
            if not resolved_runtime.librespot.status()["running"]:
                await resolved_runtime.librespot.start()
            return await resolved_runtime.spotify.play(query=request.query, uri=request.uri, device_id=request.device_id)
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.post("/v1/music/spotify/pause", dependencies=[Depends(require_admin)])
    async def spotify_pause() -> dict[str, bool]:
        try:
            return await resolved_runtime.spotify.pause()
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.post("/v1/music/spotify/resume", dependencies=[Depends(require_admin)])
    async def spotify_resume() -> dict[str, bool]:
        try:
            return await resolved_runtime.spotify.resume()
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.post("/v1/music/spotify/next", dependencies=[Depends(require_admin)])
    async def spotify_next() -> dict[str, bool]:
        try:
            return await resolved_runtime.spotify.next()
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.post("/v1/music/spotify/volume", dependencies=[Depends(require_admin)])
    async def spotify_volume(request: SpotifyVolumeRequest) -> dict[str, object]:
        try:
            return await resolved_runtime.spotify.volume(request.percent)
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    @app.get("/v1/music/spotify/playback", dependencies=[Depends(require_admin)])
    async def spotify_playback() -> dict[str, object]:
        try:
            return await resolved_runtime.spotify.playback()
        except RuntimeError as exc:
            raise HTTPException(status_code=status.HTTP_400_BAD_REQUEST, detail=str(exc)) from exc

    app.state.runtime = resolved_runtime
    return app
