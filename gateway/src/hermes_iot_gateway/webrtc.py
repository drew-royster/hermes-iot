from __future__ import annotations

import asyncio
import contextlib
import json
import logging

from aiortc import RTCConfiguration, RTCIceCandidate, RTCIceServer, RTCPeerConnection, RTCSessionDescription
from aiortc.exceptions import InvalidStateError
from aiortc.sdp import candidate_from_sdp

from .audio import PCMQueueAudioTrack
from .config import Settings
from .models import IceCandidateRequest, WebRTCOfferRequest, WebRTCOfferResponse, IceServerConfig
from .registry import InMemoryRegistry
from .session import GatewaySessionManager

logger = logging.getLogger(__name__)


class WebRTCService:
    def __init__(self, settings: Settings, registry: InMemoryRegistry, sessions: GatewaySessionManager) -> None:
        self._settings = settings
        self._registry = registry
        self._sessions = sessions

    async def accept_offer(self, device_id: str, request: WebRTCOfferRequest) -> WebRTCOfferResponse:
        ice_servers = [RTCIceServer(urls=[url]) for url in self._settings.default_ice_servers]
        pc = RTCPeerConnection(configuration=RTCConfiguration(iceServers=ice_servers))
        existing = await self._registry.get_session_for_device(device_id)
        previous_peer = existing.peer if existing else None
        session = await self._sessions.create_session(device_id)
        session.peer = pc
        await self._registry.upsert_session(session)

        if previous_peer is not None and previous_peer is not pc:
            with contextlib.suppress(Exception):
                await previous_peer.close()

        outgoing_track = PCMQueueAudioTrack(sample_rate=self._settings.deepgram_aura_sample_rate, channels=1)
        session.output_track = outgoing_track

        @pc.on("datachannel")
        def on_datachannel(channel) -> None:
            logger.info("Data channel opened for %s", device_id)

            async def send_json(payload: dict) -> None:
                with contextlib.suppress(InvalidStateError):
                    channel.send(json.dumps(payload))

            asyncio.create_task(self._sessions.bind_sender(session.session_id, send_json))

            @channel.on("message")
            def on_message(message: str) -> None:
                try:
                    parsed = json.loads(message)
                except json.JSONDecodeError:
                    logger.warning("Ignoring invalid data channel payload: %s", message)
                    return
                asyncio.create_task(self._sessions.handle_control_message(session.session_id, parsed))

        @pc.on("connectionstatechange")
        async def on_connectionstatechange() -> None:
            logger.info("Peer state for %s: %s", device_id, pc.connectionState)
            if pc.connectionState in {"failed", "closed", "disconnected"}:
                current = await self._registry.get_session(session.session_id)
                if current is None or current.peer is not pc:
                    with contextlib.suppress(Exception):
                        await pc.close()
                    return
                await self._sessions.interrupt(device_id, pc.connectionState)
                await self._registry.mark_disconnected(session.session_id)
                await self._sessions.detach_session(session.session_id)
                await pc.close()

        @pc.on("track")
        def on_track(track) -> None:
            logger.info("Received %s track from %s", track.kind, device_id)
            if track.kind == "audio":
                asyncio.create_task(self._sessions.attach_audio_track(session.session_id, track))
                asyncio.create_task(self._sessions.attach_output_track(session.session_id, outgoing_track.push_pcm))

        await pc.setRemoteDescription(RTCSessionDescription(sdp=request.sdp, type=request.type))
        attached_outgoing_track = False
        for transceiver in pc.getTransceivers():
            if transceiver.kind != "audio":
                continue
            transceiver.direction = "sendrecv"
            transceiver.sender.replaceTrack(outgoing_track)
            attached_outgoing_track = True
            break
        if not attached_outgoing_track:
            logger.warning("No negotiated audio transceiver for %s; adding outbound audio track", device_id)
            pc.addTrack(outgoing_track)
        answer = await pc.createAnswer()
        await pc.setLocalDescription(answer)

        return WebRTCOfferResponse(
            type="answer",
            sdp=pc.localDescription.sdp,
            session_id=session.session_id,
            conversation=session.conversation,
            ice_servers=[IceServerConfig(urls=[url]) for url in self._settings.default_ice_servers],
        )

    async def add_ice_candidate(self, device_id: str, request: IceCandidateRequest) -> bool:
        session = await self._registry.get_session_for_device(device_id)
        if not session or session.peer is None:
            return False
        candidate = candidate_from_sdp(request.candidate)
        ice_candidate = RTCIceCandidate(
            component=candidate.component,
            foundation=candidate.foundation,
            ip=candidate.ip,
            port=candidate.port,
            priority=candidate.priority,
            protocol=candidate.protocol,
            type=candidate.type,
            sdpMid=request.sdp_mid,
            sdpMLineIndex=request.sdp_mline_index,
            tcpType=getattr(candidate, "tcpType", None),
        )
        await session.peer.addIceCandidate(ice_candidate)
        return True
