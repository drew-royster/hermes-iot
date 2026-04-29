import asyncio
from contextlib import asynccontextmanager
from tempfile import TemporaryDirectory

import httpx
from fastapi import FastAPI
from fastapi.responses import StreamingResponse

from hermes_iot_gateway.config import Settings
from hermes_iot_gateway.runtime import create_app, create_runtime
from hermes_iot_gateway.simulator import DeviceScenario, run_simulator_sequence, run_simulator_session


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
async def _gateway_client(hermes_app: FastAPI | None = None):
    hermes_app = hermes_app or _fake_hermes_app()
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
            HERMES_IOT_CONVERSATION_MODE="device",
            HERMES_IOT_WAKE_WORD_ENABLED=False,
            HERMES_IOT_SPOTIFY_TOKEN_PATH=f"{tmpdir}/spotify_auth.json",
        )
        runtime = create_runtime(settings, hermes_http_client=hermes_client)
        app = create_app(runtime)
        await runtime.initialize()
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://testserver") as client:
            yield client
        await runtime.close()


async def _run_integration() -> None:
    async with _gateway_client() as client:
        result = await run_simulator_session(
            client=client,
            gateway_url="http://testserver",
            device_id="echo-e2e",
            prompt="Say hello",
            wait_seconds=10,
        )

        state_sequence = [
            message["payload"]["state"]
            for message in result.messages
            if message["type"] == "assistant.state"
        ]
        text_deltas = [
            message["payload"]["text"]
            for message in result.messages
            if message["type"] == "assistant.text.delta"
        ]
        tool_phases = [
            message["payload"]["phase"]
            for message in result.messages
            if message["type"] == "tool.progress"
        ]

        assert result.answer["conversation"] == "iot:echo-e2e"
        assert "listening" in state_sequence
        assert "thinking" in state_sequence
        assert "tool" in state_sequence
        assert "speaking" in state_sequence
        assert state_sequence[-1] == "listening"
        assert "".join(text_deltas) == "Hello from fake Hermes"
        assert tool_phases == ["call", "output"]


def test_end_to_end_webrtc_roundtrip() -> None:
    asyncio.run(_run_integration())


async def _run_reconnect_integration() -> None:
    async with _gateway_client() as client:
        results = await run_simulator_sequence(
            client=client,
            gateway_url="http://testserver",
            device_id="echo-reconnect",
            wait_seconds=10,
            scenario=DeviceScenario(
                prompt="Reconnect me",
                volume=35,
                reconnects=1,
                reconnect_pause=0.1,
            ),
        )
        assert len(results) == 2
        assert results[0].answer["session_id"] == results[1].answer["session_id"]
        assert results[0].answer["conversation"] == "iot:echo-reconnect"
        assert results[1].answer["conversation"] == "iot:echo-reconnect"


def test_simulator_reconnect_reuses_gateway_session() -> None:
    asyncio.run(_run_reconnect_integration())


async def _run_interrupt_integration() -> None:
    async with _gateway_client(_fake_hermes_app(text="Too slow", delay_seconds=0.3)) as client:
        results = await run_simulator_sequence(
            client=client,
            gateway_url="http://testserver",
            device_id="echo-interrupt",
            wait_seconds=10,
            scenario=DeviceScenario(
                prompt="Interrupt me",
                muted=True,
                interrupt_after=0.05,
            ),
        )
        result = results[0]
        state_sequence = [
            message["payload"]["state"]
            for message in result.messages
            if message["type"] == "assistant.state"
        ]
        text_deltas = [
            message["payload"]["text"]
            for message in result.messages
            if message["type"] == "assistant.text.delta"
        ]
        assert state_sequence == ["listening", "thinking", "idle", "listening"]
        assert text_deltas == []


def test_simulator_interrupt_returns_device_to_listening() -> None:
    asyncio.run(_run_interrupt_integration())


async def _run_debug_webrtc_ui() -> None:
    async with _gateway_client() as client:
        page = await client.get("/debug/webrtc")
        script = await client.get("/debug/webrtc/assets/app.js")
        styles = await client.get("/debug/webrtc/assets/styles.css")

        assert page.status_code == 200
        assert "Browser voice device" in page.text
        assert "Use microphone" in page.text
        assert "no push-to-talk" in page.text
        assert "/v1/pair/claim" in script.text
        assert "assistant.state" in script.text
        assert "audio.input.level" in script.text
        assert "createSilentAudioStream" in script.text
        assert styles.status_code == 200


def test_debug_webrtc_ui_is_served() -> None:
    asyncio.run(_run_debug_webrtc_ui())


async def _run_librespot_status_route() -> None:
    async with _gateway_client() as client:
        response = await client.get("/v1/music/librespot", headers={"X-Admin-Key": "dev-admin-key"})

        assert response.status_code == 200
        payload = response.json()
        assert payload["autostart_enabled"] is False
        assert payload["running"] is False
        assert payload["device_name"] == "Hermes Echo Pyramid"


def test_librespot_status_route_is_served() -> None:
    asyncio.run(_run_librespot_status_route())


async def _run_spotify_status_route() -> None:
    async with _gateway_client() as client:
        response = await client.get("/v1/music/spotify/status", headers={"X-Admin-Key": "dev-admin-key"})

        assert response.status_code == 200
        payload = response.json()
        assert payload["configured"] is False
        assert payload["authenticated"] is False
        assert payload["device_name"] == "Hermes Echo Pyramid"


def test_spotify_status_route_is_served() -> None:
    asyncio.run(_run_spotify_status_route())
