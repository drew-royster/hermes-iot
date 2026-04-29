# hermes-iot

Hermes-native WebRTC voice gateway workbench for embedded devices, starting with the M5Stack Echo Pyramid.

Current speech target: Deepgram Flux for conversational STT/turn detection plus Deepgram Aura streaming TTS, with Hermes remaining the agent brain.

## Repository layout

- `gateway/`: Python gateway service for pairing, signaling, session state, Hermes Responses streaming, and device command dispatch.
- `protocol/`: Versioned control-channel contract and message schema.
- `firmware/echo-pyramid/`: ESP-IDF Echo Pyramid client scaffold with board, state, and transport seams.
- `plugin/hermes_iot/`: Hermes plugin and toolset assets for device context and low-risk device controls.

## Current status

This repository starts from the "skateboard" stage of the plan:

- WebRTC-first backend skeleton with explicit signaling endpoints
- SQLite-backed device and session metadata
- Hermes `/v1/responses` streaming connector
- pluggable speech runtime seam with Deepgram Flux/Aura provider wiring
- data-channel control/state message contract
- local WebRTC simulator for protocol bring-up without hardware
- app/runtime factory for embedding the gateway as a Hermes-adapter-ready service
- Echo Pyramid firmware scaffold with HAL and state machine seams
- Hermes plugin scaffold for device metadata and command tools

Audio transport and session flow are now structured for always-on full duplex. The gateway can keep listening continuously, route recognized turns into Hermes, and stream TTS over the outbound WebRTC audio track. The remaining gap is live-provider validation against real Deepgram credentials plus real Echo Pyramid board/audio integration.

## First-class install path

The gateway now autodiscovers its config from your Hermes profile:

- `~/.hermes/hermes_iot.yaml` for non-secret gateway settings
- `~/.hermes/hermes_iot.env` for secrets such as API keys
- `~/.hermes/plugins/hermes_iot/` for the Hermes plugin assets

The intended install flow is:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e .[dev]
hermes-iot-setup --force
hermes-iot-doctor
hermes-iot-gateway
```

`hermes-iot-setup` installs the plugin, writes the Hermes-profile config, and creates a stable default database path under `~/.hermes/iot/hermes_iot.db`.

## Backend quick start

1. Create a virtualenv and install the package:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -e .[dev]
cp .env.example .env
```

2. Configure the gateway environment directly if you do not want to use the Hermes profile install path:

```bash
export HERMES_IOT_ADMIN_KEY=dev-admin-key
export HERMES_API_BASE_URL=http://127.0.0.1:8642/v1
export HERMES_API_KEY=change-me-local-dev
export HERMES_IOT_STATE_DB=./var/hermes_iot.db
export DEEPGRAM_API_KEY=your-deepgram-api-key
export HERMES_IOT_STT_PROVIDER=deepgram
export HERMES_IOT_TTS_PROVIDER=deepgram
```

3. Run the gateway:

```bash
hermes-iot-gateway
```

4. Pair a device:

```bash
curl -X POST http://127.0.0.1:8787/v1/pair/claim \
  -H 'Content-Type: application/json' \
  -d '{
    "device_id": "echo-pyramid-dev",
    "firmware_version": "0.0.1",
    "capabilities": ["speaker", "mic", "touch", "rgb"]
  }'
```

The returned `auth_token` is used as a bearer token for signaling and device-scoped APIs.

## Browser WebRTC tester

With the gateway running, open:

```text
http://127.0.0.1:8787/debug/webrtc
```

The page acts as a browser-backed voice device: it opens a microphone WebRTC session, streams speech to the gateway STT path, plays gateway TTS audio through the browser, shows Flux transcript events, and exposes the same control-channel events used by firmware. Use `127.0.0.1` or another secure origin for microphone permissions; plain LAN `http://` origins are usually blocked by browsers.

If you only want to test signaling, the data channel, and Hermes text turns, uncheck `Use microphone`. The tester will send a silent generated audio track and you can drive turns with `Debug Text`.

## Hermes install path

Install the Hermes plugin assets into your local Hermes profile only:

```bash
source .venv/bin/activate
hermes-iot-install-plugin
```

That copies the plugin into `~/.hermes/plugins/hermes_iot`. For the full first-class setup flow, use `hermes-iot-setup` instead. See [plugin/README.md](/Users/drewroyster/Documents/hermes-iot/plugin/README.md) and [docs/native-hermes-gateway.md](/Users/drewroyster/Documents/hermes-iot/docs/native-hermes-gateway.md).

## Simulator quick start

With the gateway running, exercise the control plane and Hermes loop without hardware:

```bash
source .venv/bin/activate
hermes-iot-sim --prompt "Turn on the office lights and tell me what happened"
```

The simulator pairs a virtual device, opens a WebRTC session, sends `hello`, and prints control-channel messages from the gateway.

For a fully local Echo Pyramid / ESP32 self-test with no running gateway or Hermes server required:

```bash
make sim-esp32
```

That boots an in-process fake Hermes `/v1/responses` backend, an in-process gateway runtime, and a protocol-faithful simulated Echo Pyramid client.

The simulator can also exercise more realistic device behavior:

```bash
source .venv/bin/activate
hermes-iot-sim --self-test \
  --device-profile echo-pyramid \
  --prompt "Reconnect and interrupt me" \
  --muted \
  --volume 42 \
  --reconnects 1 \
  --interrupt-after 0.05
```

That verifies the same simulated device can reconnect onto the same gateway session and can interrupt an in-flight assistant turn back to `listening`.

During live runs, the gateway now also tracks the latest `device.state`, `mute.set`, and `volume.set` messages and exposes them through the admin device summary endpoints. That gives you a concrete “what does the device think its state is?” debug surface.

## Spotify Connect backend

The backend can launch `librespot` as a local Spotify Connect target and route raw PCM into the connected Echo Pyramid WebRTC output track. There is intentionally no Mac-audio fallback in this mode; the device must have an active WebRTC output track.

Install `librespot`:

```bash
brew install librespot
```

Start the gateway, then start the Spotify Connect target:

```bash
curl -X POST http://127.0.0.1:8787/v1/music/librespot/start \
  -H 'X-Admin-Key: dev-admin-key'
```

Check status and recent logs:

```bash
curl http://127.0.0.1:8787/v1/music/librespot \
  -H 'X-Admin-Key: dev-admin-key'
```

Stop it:

```bash
curl -X POST http://127.0.0.1:8787/v1/music/librespot/stop \
  -H 'X-Admin-Key: dev-admin-key'
```

Useful environment knobs:

```bash
export HERMES_IOT_LIBRESPOT_ENABLED=true
export HERMES_IOT_LIBRESPOT_NAME="Hermes Echo Pyramid"
export HERMES_IOT_LIBRESPOT_BACKEND=pipe
export HERMES_IOT_LIBRESPOT_TARGET_DEVICE_ID=echo-pyramid-dev
export HERMES_IOT_LIBRESPOT_CACHE_DIR="$HOME/.hermes/iot/librespot"
```

### Spotify Web API control

Spotify search and playback control use Spotify's Web API on top of the `librespot` Connect device. This requires Spotify Premium.

Create a Spotify Developer app, then add this redirect URI:

```text
http://127.0.0.1:8787/v1/music/spotify/callback
```

Configure the gateway with the app's client ID:

```bash
export HERMES_IOT_SPOTIFY_CLIENT_ID=your-spotify-client-id
export HERMES_IOT_SPOTIFY_REDIRECT_URI=http://127.0.0.1:8787/v1/music/spotify/callback
export HERMES_IOT_SPOTIFY_DEVICE_NAME="Hermes Echo Pyramid"
```

With the gateway running, get the login URL:

```bash
curl http://127.0.0.1:8787/v1/music/spotify/login \
  -H 'X-Admin-Key: dev-admin-key'
```

Open the returned `auth_url` in a browser and approve access. The callback stores a refresh token under:

```text
~/.hermes/iot/spotify_auth.json
```

Search and play through the gateway:

```bash
curl 'http://127.0.0.1:8787/v1/music/spotify/search?q=shake%20it%20off%20taylor%20swift' \
  -H 'X-Admin-Key: dev-admin-key'
```

```bash
curl -X POST http://127.0.0.1:8787/v1/music/spotify/play \
  -H 'X-Admin-Key: dev-admin-key' \
  -H 'Content-Type: application/json' \
  -d '{"query":"Shake It Off Taylor Swift"}'
```

The Hermes IoT plugin exposes this as narrow tools: `iot_media_search`, `iot_media_play`, `iot_media_pause`, `iot_media_resume`, `iot_media_next`, `iot_media_volume`, and `iot_media_status`.

## Testing loop

The repo now has a real end-to-end loop that does not depend on live Hermes or hardware:

```bash
make test-loop
```

That integration test boots:

- a fake Hermes `/v1/responses` SSE backend
- the Hermes IoT gateway via the runtime/app factory
- a simulated always-listening WebRTC device client

and verifies the full signaling/data-channel/Responses round-trip.

For the full local suite:

```bash
make test
```

To verify that a Hermes-profile install is sane before you bring a device online:

```bash
make doctor
```

## Protocol notes

- WebRTC handles audio media.
- A reliable ordered data channel carries control, state, text deltas, tool-progress, and device-command messages.
- `debug.user_text` exists only for local testing; production turns should come from the upstream audio track via Flux.

See `protocol/README.md` and `IMPLEMENTATION_PLAN.md` for the active contract and work checklist.
