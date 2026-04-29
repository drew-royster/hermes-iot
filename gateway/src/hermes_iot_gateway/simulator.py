from __future__ import annotations

import argparse
import asyncio
import json
import logging
from contextlib import asynccontextmanager
from dataclasses import dataclass
from fractions import Fraction
from tempfile import TemporaryDirectory

import httpx
from fastapi import FastAPI
from fastapi.responses import StreamingResponse
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.exceptions import InvalidStateError
from aiortc.mediastreams import AudioStreamTrack
from av import AudioFrame

from .config import Settings
from .runtime import create_app, create_runtime


logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class DeviceProfile:
    name: str
    firmware_version: str
    sample_rate_hz: int
    codec: str
    capabilities: list[str]
    metadata: dict[str, object]


@dataclass(slots=True)
class SimulatorResult:
    messages: list[dict]
    answer: dict


@dataclass(frozen=True, slots=True)
class DeviceScenario:
    prompt: str
    muted: bool | None = None
    volume: int | None = None
    interrupt_after: float | None = None
    reconnects: int = 0
    reconnect_pause: float = 0.2


class NullMicTrack(AudioStreamTrack):
    kind = "audio"

    def __init__(self) -> None:
        super().__init__()
        self._sample_rate = 48000
        self._pts = 0

    async def recv(self) -> AudioFrame:
        await asyncio.sleep(0.02)
        frame = AudioFrame(format="s16", layout="mono", samples=960)
        for plane in frame.planes:
            plane.update(b"\x00" * plane.buffer_size)
        frame.sample_rate = self._sample_rate
        frame.time_base = Fraction(1, self._sample_rate)
        frame.pts = self._pts
        self._pts += 960
        return frame


DEVICE_PROFILES: dict[str, DeviceProfile] = {
    "echo-pyramid": DeviceProfile(
        name="echo-pyramid",
        firmware_version="sim-esp32s3-0.1.0",
        sample_rate_hz=16000,
        codec="pcm16",
        capabilities=["speaker", "mic", "touch", "rgb"],
        metadata={
            "simulator": True,
            "board": "m5stack-echo-pyramid",
            "soc": "esp32s3",
            "transport_stack": "aiortc",
        },
    ),
}


def _send_channel_message(channel, payload: dict) -> None:
    try:
        channel.send(json.dumps(payload))
    except InvalidStateError:
        logger.debug("Dropping simulator payload on closed data channel: %s", payload.get("type"))


def _device_hello(profile: DeviceProfile, device_id: str) -> dict:
    return {
        "device_id": device_id,
        "firmware_version": profile.firmware_version,
        "transport": "webrtc",
        "sample_rate_hz": profile.sample_rate_hz,
        "codec": profile.codec,
        "capabilities": profile.capabilities,
        "metadata": {**profile.metadata, "device_id": device_id},
    }


def _fake_hermes_app(*, text: str = "Hello from fake Hermes", delay_seconds: float = 0.0) -> FastAPI:
    app = FastAPI()

    @app.post("/v1/responses")
    async def responses():
        async def stream():
            yield 'event: response.created\ndata: {"response":{"id":"resp_fake"}}\n\n'
            yield (
                'event: response.output_item.added\n'
                'data: {"item":{"type":"function_call","name":"iot_set_led","arguments":"{}",'
                '"call_id":"call_fake"}}\n\n'
            )
            yield (
                'event: response.output_item.done\n'
                'data: {"item":{"type":"function_call_output","call_id":"call_fake","output":"ok"}}\n\n'
            )
            if delay_seconds:
                await asyncio.sleep(delay_seconds)
            yield f'event: response.output_text.delta\ndata: {{"delta":"{text}"}}\n\n'
            yield 'event: response.completed\ndata: {"response":{"id":"resp_fake"}}\n\n'

        return StreamingResponse(stream(), media_type="text/event-stream")

    return app


@asynccontextmanager
async def self_test_client(*, hermes_text: str = "Hello from fake Hermes", delay_seconds: float = 0.0):
    hermes_app = _fake_hermes_app(text=hermes_text, delay_seconds=delay_seconds)
    hermes_client = httpx.AsyncClient(
        transport=httpx.ASGITransport(app=hermes_app),
        base_url="http://fake-hermes",
        timeout=30,
    )
    with TemporaryDirectory() as tmpdir:
        settings = Settings(
            HERMES_IOT_HOST="testserver",
            HERMES_IOT_PORT=8787,
            HERMES_IOT_ADMIN_KEY="dev-admin-key",
            HERMES_IOT_STATE_DB=f"{tmpdir}/state.db",
            HERMES_API_BASE_URL="http://fake-hermes/v1",
            HERMES_API_KEY="fake-key",
        )
        runtime = create_runtime(settings, hermes_http_client=hermes_client)
        app = create_app(runtime)
        await runtime.initialize()
        async with httpx.AsyncClient(
            transport=httpx.ASGITransport(app=app),
            base_url="http://testserver",
            timeout=30,
        ) as client:
            yield client
        await runtime.close()


async def run_simulator_session(
    *,
    client: httpx.AsyncClient,
    gateway_url: str,
    device_id: str,
    prompt: str,
    wait_seconds: float,
    device_profile: str = "echo-pyramid",
    auth_token: str | None = None,
    muted: bool | None = None,
    volume: int | None = None,
    interrupt_after: float | None = None,
    reconnect_index: int = 0,
) -> SimulatorResult:
    profile = DEVICE_PROFILES[device_profile]
    hello_payload = _device_hello(profile, device_id)
    token = auth_token
    if token is None:
        claim = await client.post(
            f"{gateway_url}/v1/pair/claim",
            json={
                "device_id": device_id,
                "firmware_version": profile.firmware_version,
                "capabilities": profile.capabilities,
            },
        )
        claim.raise_for_status()
        claim_data = claim.json()
        token = claim_data["auth_token"]

    pc = RTCPeerConnection()
    channel = pc.createDataChannel("control")
    pc.addTrack(NullMicTrack())
    received_messages: list[dict] = []
    listening_seen = asyncio.Event()
    local_device_state = {
        "muted": bool(muted) if muted is not None else False,
        "volume": volume if volume is not None else 75,
        "reconnect_index": reconnect_index,
        "profile": device_profile,
    }

    @channel.on("open")
    def on_open() -> None:
        logger.info("control channel open")
        _send_channel_message(
            channel,
            {
                "type": "hello",
                "payload": hello_payload,
            },
        )
        _send_channel_message(channel, {"type": "device.state", "payload": local_device_state})
        if muted is not None:
            _send_channel_message(channel, {"type": "mute.set", "payload": {"muted": muted}})
        if volume is not None:
            _send_channel_message(channel, {"type": "volume.set", "payload": {"volume": volume}})

        async def inject_text() -> None:
            await asyncio.sleep(0.25)
            _send_channel_message(
                channel,
                {
                    "type": "debug.user_text",
                    "payload": {
                        "text": prompt,
                        "hello": hello_payload["metadata"],
                    },
                },
            )

        async def inject_interrupt() -> None:
            if interrupt_after is None:
                return
            delay = interrupt_after + (0.25 if prompt else 0.0)
            await asyncio.sleep(delay)
            _send_channel_message(channel, {"type": "interrupt", "payload": {"reason": "simulator"}})

        if prompt:
            asyncio.create_task(inject_text())
        asyncio.create_task(inject_interrupt())

    @channel.on("message")
    def on_message(message: str) -> None:
        parsed = json.loads(message)
        received_messages.append(parsed)
        payload = parsed.get("payload", {})
        if parsed.get("type") == "assistant.state" and payload.get("state") == "listening":
            if any(
                msg.get("type") == "assistant.text.delta"
                or (
                    msg.get("type") == "assistant.state"
                    and msg.get("payload", {}).get("state") in {"thinking", "tool", "speaking", "idle"}
                )
                for msg in received_messages
            ):
                listening_seen.set()

    offer = await pc.createOffer()
    await pc.setLocalDescription(offer)

    response = await client.post(
        f"{gateway_url}/v1/devices/{device_id}/webrtc/offer",
        headers={"Authorization": f"Bearer {token}"},
        json={
            "type": "offer",
            "sdp": pc.localDescription.sdp,
            "hello": hello_payload,
        },
    )
    response.raise_for_status()
    answer = response.json()
    await pc.setRemoteDescription(RTCSessionDescription(sdp=answer["sdp"], type=answer["type"]))

    try:
        await asyncio.wait_for(listening_seen.wait(), timeout=wait_seconds)
    finally:
        await pc.close()
    return SimulatorResult(messages=received_messages, answer=answer)


async def run_simulator_sequence(
    *,
    client: httpx.AsyncClient,
    gateway_url: str,
    device_id: str,
    wait_seconds: float,
    device_profile: str = "echo-pyramid",
    scenario: DeviceScenario,
) -> list[SimulatorResult]:
    profile = DEVICE_PROFILES[device_profile]
    claim = await client.post(
        f"{gateway_url}/v1/pair/claim",
        json={
            "device_id": device_id,
            "firmware_version": profile.firmware_version,
            "capabilities": profile.capabilities,
        },
    )
    claim.raise_for_status()
    claim_data = claim.json()
    token = claim_data["auth_token"]

    results: list[SimulatorResult] = []
    attempts = scenario.reconnects + 1
    for reconnect_index in range(attempts):
        results.append(
            await run_simulator_session(
                client=client,
                gateway_url=gateway_url,
                device_id=device_id,
                prompt=scenario.prompt,
                wait_seconds=wait_seconds,
                device_profile=device_profile,
                auth_token=token,
                muted=scenario.muted,
                volume=scenario.volume,
                interrupt_after=scenario.interrupt_after,
                reconnect_index=reconnect_index,
            )
        )
        if reconnect_index != attempts - 1:
            await asyncio.sleep(scenario.reconnect_pause)
    return results


def _print_result(result: SimulatorResult, *, attempt: int = 1) -> None:
    state_sequence = [
        message["payload"]["state"]
        for message in result.messages
        if message["type"] == "assistant.state"
    ]
    text = "".join(
        message["payload"]["text"]
        for message in result.messages
        if message["type"] == "assistant.text.delta"
    )
    print(f"attempt={attempt}")
    print(f"session_id={result.answer['session_id']}")
    print(f"conversation={result.answer['conversation']}")
    print(f"state_sequence={state_sequence}")
    print(f"assistant_text={text}")
    for message in result.messages:
        print(json.dumps(message))


async def run_simulator(args) -> None:
    if args.self_test:
        client_manager = self_test_client(
            delay_seconds=0.4 if args.interrupt_after is not None else 0.0,
        )
        gateway_url = "http://testserver"
    else:
        client_manager = httpx.AsyncClient(timeout=30)
        gateway_url = args.gateway_url

    async with client_manager as client:
        scenario = DeviceScenario(
            prompt=args.prompt,
            muted=args.muted,
            volume=args.volume,
            interrupt_after=args.interrupt_after,
            reconnects=args.reconnects,
            reconnect_pause=args.reconnect_pause,
        )
        results = await run_simulator_sequence(
            client=client,
            gateway_url=gateway_url,
            device_id=args.device_id,
            wait_seconds=args.wait_seconds,
            device_profile=args.device_profile,
            scenario=scenario,
        )
        print(f"attempts={len(results)}")
        print(f"session_ids={[result.answer['session_id'] for result in results]}")
        for index, result in enumerate(results, start=1):
            _print_result(result, attempt=index)


def main() -> None:
    parser = argparse.ArgumentParser(description="Hermes IoT simulated Echo Pyramid / ESP32 WebRTC client")
    parser.add_argument("--gateway-url", default="http://127.0.0.1:8787")
    parser.add_argument("--device-id", default="echo-pyramid-sim")
    parser.add_argument("--prompt", default="What can you do through this device?")
    parser.add_argument("--wait-seconds", type=float, default=10.0)
    parser.add_argument("--device-profile", choices=sorted(DEVICE_PROFILES), default="echo-pyramid")
    parser.add_argument("--muted", action="store_true", help="Emit an initial mute event from the simulated device")
    parser.add_argument("--volume", type=int, default=None, help="Emit a volume change from the simulated device")
    parser.add_argument(
        "--interrupt-after",
        type=float,
        default=None,
        help="Send an interrupt this many seconds after the prompted turn starts",
    )
    parser.add_argument("--reconnects", type=int, default=0, help="Reconnect the same simulated device this many times")
    parser.add_argument("--reconnect-pause", type=float, default=0.2)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run against an in-process fake Hermes backend and in-process gateway",
    )
    args = parser.parse_args()
    asyncio.run(run_simulator(args))
