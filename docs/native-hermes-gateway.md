# Native Hermes Gateway Notes

This repo is moving from standalone gateway service to a Hermes-native IoT platform. The standalone service remains useful for simulation and regression tests, but the hackathon target is a first-class Hermes gateway that gives an embedded device a body.

The product goal is a fast home-running assistant for moms: wake on `hey willow`, answer quickly, set timers reliably, load skills when needed, and expose practical household workflows without turning a kitchen speaker into an unsafe general-purpose terminal.

## Current runtime mapping

- Device identity maps to a stable Hermes conversation key: `iot:<device_id>`.
- Gateway sessions are reused per device, matching Hermes' expectation that one chat/session has continuity and interrupt semantics.
- The speech path is designed for full duplex: Deepgram Flux handles streaming STT and turn state, Hermes handles reasoning/tools/history, and Aura handles streaming TTS.
- The runtime exposes a clean boundary in `hermes_iot_gateway.runtime`:
  - `create_runtime()` wires storage, speech, Hermes Responses client, session manager, and WebRTC service.
  - `create_app()` wraps that runtime in an installable HTTP gateway.

This seam is now being moved behind a Hermes platform adapter.

## Hermes-native target

The intended native landing zone is:

- Hermes platform adapter owns transport/signaling and converts WebRTC/data-channel device events into Hermes `MessageEvent`-style inputs.
- Hermes gateway session store owns interruption and active-turn coordination.
- Hermes plugin/toolset remains the extension layer for device-aware behavior.
- Proxy mode remains valid for split deployments where the WebRTC-facing process is near devices and the main Hermes agent runs elsewhere.
- If Deepgram remains the speech engine, the adapter boundary should treat Flux/Aura as transport-adjacent speech services, not as the agent brain. Hermes stays responsible for tool use, memory, and prompt policy.
- Wake/sleep state belongs at the gateway/device boundary: asleep devices should ignore ambient transcripts until `hey willow`, then reset a roughly 30-second activity window on user turns, assistant speech, and local device interactions.
- The default IoT tool surface should stay narrow and fast: time, named timers, device feedback, and skill loading. Additional smart-home or high-risk tools should be explicit opt-ins.

## Install path today

The current repo now has a Hermes-profile-first install path:

1. Install the Python package in editable mode.
2. Run `hermes-iot-setup --force`.
3. That writes `~/.hermes/hermes_iot.yaml`, `~/.hermes/hermes_iot.env`, installs `~/.hermes/plugins/hermes_iot`, and creates a stable default DB path under `~/.hermes/iot/`.
4. Run `hermes-iot-doctor` to verify plugin install, config presence, API keys, DB path, and Hermes `/v1/responses` reachability.
5. Run `hermes-iot-gateway`. It now auto-loads config from the Hermes profile.
6. Use the `hermes-iot` plugin/toolset inside Hermes sessions.

That still leaves the runtime as a separate process. The WIP native path adds `Platform.IOT`, runs the WebRTC transport inside Hermes, and dispatches device speech turns through Hermes' in-process message handler instead of `/v1/responses`.

## Promotion checklist

- Finish and live-test the `Platform.IOT` adapter installer for local Hermes checkouts.
- Replace the standalone FastAPI route layer with Hermes platform-adapter registration where Hermes supports it directly.
- Keep `runtime.py`, `session.py`, `speech.py`, and `webrtc.py` as the reusable gateway core.
- Replace direct `/v1/responses` wiring with Hermes-native dispatch where available, while preserving proxy-mode support for split deployment.
- Keep the new Hermes-profile config loader, setup command, and doctor checks as the operator-facing surface even after the runtime moves behind Hermes registration.
- Add `hey willow` wake-word gating and a 30-second sleep timeout before demo hardening.
- Add/get-expose get-time, named timers, and skill-loading checks as the first native IoT workflows.
