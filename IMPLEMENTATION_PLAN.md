# Hermes IoT Implementation Plan

This file is the working checklist for the repo. It should be updated as milestones land.

## North star

Win the Hermes creative hackathon by making an excellent Hermes IoT device for moms running a home. The device should feel fast, ambient, safe, and practically useful in household chaos: wake on `hey willow`, answer quickly, set timers reliably, support routines/skills, give clear visual/audio feedback, and keep Hermes' tool power available without exposing risky capabilities by default.

## Core milestones

- [x] Create the repository scaffold and save the implementation plan in-repo.
- [x] Add a Python gateway skeleton with FastAPI, pairing, signaling, session registry, and Hermes Responses integration.
- [x] Define the v1 control-channel protocol and publish a schema in `protocol/`.
- [x] Add a Hermes plugin scaffold with low-risk IoT tools and device-context injection.
- [x] Add an Echo Pyramid ESP-IDF scaffold with state machine and HAL seams.
- [x] Replace debug text turns with real STT from upstream audio.
- [x] Add streamed TTS playback over the WebRTC downstream audio path.
- [x] Replace in-memory registries with persistent device/session storage.
- [x] Wire real Echo Pyramid board drivers, touch input, LEDs, and audio codec paths.
- [ ] Add TURN/STUN configuration, reconnect hardening, and deployment docs for remote networks.
- [x] Add a repeatable local testing loop for signaling, session state, and Hermes stream integration.
- [x] Add a Hermes-profile-native setup path with autodiscovered config, plugin install, and doctor checks.
- [x] Add a protocol-faithful simulated Echo Pyramid / ESP32 client for local no-hardware validation.
- [ ] Promote the gateway runtime boundary into a Hermes adapter or proxy-compatible package.
- [x] Add `hey willow` wake-word gating with automatic return-to-sleep after 30 seconds of inactivity.
- [x] Move the real Echo Pyramid `hey willow` wake path on-device with ESP-SR WakeNet instead of depending on server transcript gating.
- [x] Add a fast native IoT toolset with time and timers while excluding broad/high-risk tools by default.

## Stage breakdown

### Stage 1: Skateboard

- [x] Pair devices and issue scoped auth tokens.
- [x] Accept WebRTC offers and return answers.
- [x] Maintain per-device sessions and interrupt state.
- [x] Stream Hermes `/v1/responses` events into normalized gateway events.
- [x] Dispatch device commands over the control channel.
- [x] Exercise the full loop with debug text over the data channel.

### Stage 2: Bike

- [x] Decode inbound audio and feed a pluggable STT pipeline.
- [x] Convert assistant text to TTS audio frames.
- [ ] Reflect tool-running, speaking, and error states on the device.
- [x] Add reconnect and session-resume behavior.

### Stage 3: Motorcycle

- [x] Add claim lifecycle management, admin APIs, and persistent metadata.
- [x] Persist claimed device bootstrap data on the Echo Pyramid.
- [ ] Add board-specific Echo Pyramid integrations for touch, mute, and RGB state.
- [ ] Harden interrupt and barge-in semantics.
- [x] Add a simulated client harness for automated end-to-end tests.
- [x] Add a repeatable device bring-up loop on real Echo Pyramid hardware.

### Stage 4: Car

- [ ] Add remote deployment documentation and proxy-mode packaging.
- [ ] Add native Hermes adapter boundaries for platform/session mapping.
- [ ] Expand device classes beyond Echo Pyramid without changing the protocol.

## Current status

- The Echo Pyramid firmware now builds, flashes, joins Wi-Fi, claims with the gateway, and completes a real spoken-turn loop against the local Hermes IoT gateway.
- Real hardware has verified the following path end-to-end:
  - device boot and audio bring-up
  - Wi-Fi association on the local LAN
  - gateway `/health`
  - pair/claim bootstrap
  - WebRTC offer/answer exchange
  - DTLS handshake
  - SCTP association
  - control data channel creation
  - outbound `hello`
  - inbound `assistant.state=listening`
- Real hardware has also verified the first live speech loop:
  - Pyramid mic audio reaches the gateway over WebRTC/SRTP
  - Deepgram Flux produces a spoken turn transcript
  - the gateway posts that turn to `/v1/responses`
  - the downstream TTS reply plays on the Pyramid speaker
- The repo now has a Hermes-home-native install surface:
  - `hermes-iot-setup` writes `~/.hermes/hermes_iot.yaml` and `~/.hermes/hermes_iot.env`
  - `hermes-iot-setup` installs the `hermes_iot` plugin into `~/.hermes/plugins/`
  - `hermes-iot-gateway` auto-loads that profile instead of requiring ad hoc shell exports
  - `hermes-iot-doctor` verifies plugin install, config presence, DB path, keys, and Hermes `/v1/responses` reachability
- The repo also now has a protocol-faithful simulated Echo Pyramid / ESP32 path for local validation:
  - `hermes-iot-sim --self-test --device-profile echo-pyramid`
  - `make sim-esp32`
  - optional scenario flags now cover reconnects, mute/volume events, and interrupts
  - both boot a fake Hermes SSE backend, an in-process gateway, and a virtual WebRTC device client
- The current blocker is no longer “can this connect live” or “is downlink audio intelligible.” The latest local run completed a real full path: MacBook playback near the Pyramid -> Pyramid mic -> WebRTC/SRTP -> Deepgram Flux -> Hermes `/v1/responses` -> Hermes memory tool -> streamed text -> Deepgram Aura -> Pyramid speaking state/output path.
- The mic-channel fix is now validated on hardware: the firmware follows the official M5 path by enabling ES7210 MIC1+MIC3, opening stereo capture, sending the M5-labeled mic slot upstream, and keeping the ref slot logged/reserved for future AEC-aware handling.
- The speaker/downlink fix is now validated on hardware: the firmware follows the official M5 Si5351 path for `16000 * 256 = 4096000 Hz` MCLK on CLK1 and duplicates mono speaker PCM into the official stereo I2S write layout before ES8311 output.
- The current gateway now defaults live Hermes turns to session-scoped conversations (`iot:<device>:session:<session_id>`) instead of the long-lived device conversation. A direct latency check showed the stale `iot:echo-pyramid-dev` conversation taking about 15.6 seconds to first text for a one-word reply, while fresh/session-scoped conversations respond in roughly 2-4 seconds.
- The product target is explicitly Hermes-native, not a long-term `/v1/responses` sidecar. Sidecar mode remains useful for local simulation and regression tests, but the operator flow should feel like enabling a first-class Hermes gateway.
- The intended voice UX is always-available full duplex gated by wake word, not push-to-talk: wake on `hey willow`, keep a short conversational activity window, and return to sleep after about 30 seconds of inactivity. Ambient audio while asleep should not reach Hermes as user turns.
- The Echo Pyramid firmware now uses the same ESP-SR WakeNet pattern as the known-good Pipecat reference: `wn9_heywillow_tts` is packed into a `model` flash partition, the firmware listens locally while asleep, plays the wake tone on detection, and only then requests a WebRTC session.
- Gateway-side wake gating is now off by default. It remains available for simulators or always-streaming clients, but the physical Echo Pyramid path should rely on firmware-local wake so the wake phrase does not need to traverse Deepgram/Hermes.
- Latest native-gateway live issue: an IoT voice turn inherited too much generic Hermes skill/tool behavior and attempted non-device workflows. The current decision is to make IoT a tightly scoped platform: `platform_toolsets.iot` should be `["hermes-iot", "no_mcp"]`, native IoT runs should skip memory/provider tools, and IoT prompts should explicitly avoid desktop/email/setup workflows unless a future device-specific skill is intentionally loaded.
- Safety status before stopping on 2026-04-25: the native Hermes gateway process was stopped, runtime state reported `api_server` and `iot` disconnected, and no process was listening on the local gateway/API ports. This should prevent lingering speech/WebRTC usage overnight.
- The default native IoT tool surface should stay fast and narrow: `iot_get_time`, `iot_set_timer`, and device feedback tools. Generic Hermes skills/MCP/email/desktop tooling should remain outside the IoT default until there is an intentional device-appropriate loading flow.
- Deepgram Aura downlink is now drained before the gateway returns the device to `listening`, so the UI/listening state more closely matches actual speaker playback. This reduces, but does not fully eliminate, occasional full-duplex false follow-up turns from speaker echo.
- Local Hermes authentication was repaired during live testing, then a direct `/v1/responses` smoke test verified the API path. Keep future auth repair details out of tracked docs.
- Local firmware tones have been disabled for quiet testing and idle/debug logs have been reduced. The board should now stay substantially quieter during iteration.
- Official hardware references have been cached and summarized in `docs/reference/hardware-lookup.md`.
- The M5/Xiaozhi examples have been reviewed and summarized in `docs/reference/xiaozhi-review.md`. The useful pattern is not to port Xiaozhi wholesale, but to copy its full-duplex state model, bounded audio queues, AEC gating, Opus framing assumptions, and local audio-test diagnostic loop while keeping Voice Pyramid hardware behavior aligned with M5Echo-Pyramid and ESPHome.
- The first Xiaozhi-inspired firmware pass is implemented: the Pyramid now advertises `audio_reference`, `audio_stats`, and `audio_loopback`, emits `audio.stats` telemetry over the control channel, and can run a bounded local mic-record/speaker-playback loopback via `device.command` without involving Hermes, Deepgram, or WebRTC media downlink.
- Downlink audio was narrowed to the ESP/libpeer receive boundary and then fixed:
  - direct Deepgram Aura captures at 16 kHz and 48 kHz produced valid mono signed-16 WAV files
  - a simulated WebRTC receiver captured the gateway's outbound aiortc audio as 48 kHz stereo and Deepgram STT transcribed it correctly
  - the current flashed firmware includes an RTP parser fix so libpeer strips CSRC/header-extension/padding fields before handing payload bytes to Opus
  - the current flashed firmware also fixes Si5351 CLK1/MCLK programming and ES8311 speaker I2S layout to match the M5Echo-Pyramid library

## Immediate next steps

- Keep the new hardware-output baseline pinned: Si5351 CLK1 must be configured for 4.096 MHz MCLK at 16 kHz, and speaker writes must send stereo duplicated PCM even when the gateway source is mono.
- Add a gateway/firmware capture fixture that records a few seconds of mic PCM/Opus from the live device so future STT issues can be debugged without relying on live speech timing.
- Keep the gateway downlink capture loop as a regression test: `var/sim-gateway-recv-correct.wav` proved the server-side TTS/WebRTC path is intelligible before it reaches ESP hardware.
- Validate barge-in/full-duplex behavior under real overlapping speech. Current behavior is full-duplex transport, but the gateway pauses turn submission while Hermes is thinking/speaking and resumes listening after completion.
- Continue full-duplex echo hardening without masking real quick replies:
  - keep assistant-state `speaking` active until Aura audio drains
  - keep short post-playback echo suppression
  - investigate using the ES7210 ref slot / hardware AEC path instead of relying only on transcript heuristics
  - prototype an ESP AFE/device-side AEC path behind Kconfig using the Pyramid `mic/ref` slots, following Xiaozhi's `M...R` input-format pattern
  - avoid broad "ignore all speech after playback" gates because they break natural turn-taking
- Flash and live-test the new firmware diagnostics:
  - confirm `audio.stats` arrives in the gateway session state once per second
  - trigger `device.command` with `{"type":"audio_loopback","duration_ms":2000}`
  - verify the local loopback capture sounds correct and ref/mic peaks make sense
- Keep lifting proven pieces from the prior Echo Pyramid prototype where needed, but the base Opus send/receive loop is now in place:
  - Opus decoder path
  - playback queue and prebuffer behavior
  - mic capture and Opus encoder publisher task
  - playback reset and underrun handling
- Harden the remaining live edges:
  - device-side UX should reflect audio/input progress more clearly
  - add reconnect behavior once the gateway or peer closes unexpectedly
  - validate barge-in/full-duplex behavior under real overlapping speech
- Push the Hermes-native packaging further:
  1. finish the in-process `Platform.IOT` adapter path so STT turns enter Hermes as native `MessageEvent`s instead of `/v1/responses`
  2. let the gateway boot directly from Hermes-home config without repo-local `.env` leakage
  3. keep `setup` and `doctor` as the operator-facing install surface
  4. preserve standalone sidecar/simulator mode only as a dev/test fallback
- Finish the wake/sleep state machine:
  - keep firmware-local `hey willow` as the physical-device default
  - close or idle the WebRTC session after roughly 30 seconds of inactivity
  - reset the awake timer on user turns, assistant speech, and device interactions
  - send explicit device states for `sleeping`, `awake/listening`, `thinking`, `speaking`, and `error`
- Finish IoT-specific native Hermes behavior:
  - keep `skills` out of the default IoT platform toolset
  - keep `no_mcp` enabled for IoT unless explicitly overridden
  - keep Honcho/memory providers skipped for IoT turns by default
  - add an IoT system prompt that says the assistant is a fixed home device, not a desktop/email agent
  - only allow skill loading later via an intentional, device-appropriate flow
- Add native IoT tools/workflows:
  - refine the time tool UX for spoken responses
  - refine named timer scheduling with spoken and visual completion
  - design a safe future skill-loading workflow for IoT sessions without enabling generic `skills` by default
- Once the runtime packaging is stable, graduate the gateway boundary from polished sidecar to native Hermes gateway/adapter.

## Progress log

- 2026-04-17: Initialized repository structure, backend skeleton, protocol schema, Hermes plugin scaffold, and ESP-IDF firmware scaffold.
- 2026-04-17: Created a local `.venv`, installed declared dependencies, and verified the Python gateway scaffold with `python -m compileall` and `python -m pytest gateway/tests` (`3 passed`).
- 2026-04-17: Added SQLite-backed registry persistence, a speech runtime seam, upstream audio ingest hooks, and a local WebRTC simulator CLI.
- 2026-04-17: Added session reuse on reconnect, refreshed the editable install, and verified the updated backend with `python -m pytest gateway/tests` (`5 passed`) plus `hermes-iot-sim --help`.
- 2026-04-17: Added an app/runtime factory, a fake-Hermes end-to-end WebRTC integration test, and a repeatable `make test-loop` dev path.
- 2026-04-17: Added a Hermes plugin installer CLI, native-gateway integration notes, and a realistic `ptt.start`/`ptt.stop` simulator/test flow. Current backend verification: `8 passed`.
- 2026-04-17: Pivoted the runtime to always-on full duplex, replaced the test path with `debug.user_text`, and wired Deepgram Flux/Aura provider classes into the speech runtime.
- 2026-04-17: Ported a minimal Echo Pyramid board bring-up target from the working Pipecat client reference, fixed ESP-IDF component and codec config issues, and verified a clean `idf.py build` for `esp32s3`.
- 2026-04-17: Flashed the bring-up image to `/dev/cu.usbmodem101`, added boot-time I2C scanning plus graceful audio-init failure handling, and confirmed the current hardware path reports no Echo Pyramid peripherals on SDA=38/SCL=39 instead of crashing in a reboot loop.
- 2026-04-17: After seating the AtomS3R correctly, confirmed live Echo Pyramid peripherals on the I2C bus, pinned `espressif/esp_codec_dev` back to `1.3.5` to match the known-good reference, and verified the board now reaches and completes speaker self-test without the prior I2S RX panic.
- 2026-04-17: Added menuconfig-backed Wi-Fi and gateway bootstrap modules, fixed their ESP-IDF build integration, and verified on real hardware that the board now boots, runs audio self-test, and cleanly lands in `READY (Needs WiFi)` when network provisioning is unset instead of failing the whole runtime.
- 2026-04-17: Added an NVS-backed device identity store for claimed auth/signaling/conversation data, rebuilt successfully, and reflashed the Pyramid to keep the verified bring-up image current.
- 2026-04-18: Removed the dev bootstrap token requirement from the pair-claim path with a TODO to restore production enrollment, provisioned the Pyramid for local testing, and verified a live hardware bootstrap through `/health` and `/v1/pair/claim` against a local gateway.
- 2026-04-18: Vendored the working `peer` / `srtp` / `libpeer` stack from the Pipecat Echo Pyramid reference, enabled MbedTLS DTLS and DTLS-SRTP in the firmware config, and verified a clean ESP-IDF build with the native WebRTC client linked in.
- 2026-04-18: Verified on real hardware that the Hermes client now reaches WebRTC offer/answer, DTLS, SCTP, control data channel creation, sends `hello`, and receives `assistant.state=listening` from the local gateway.
- 2026-04-18: Patched vendored libpeer SDP interop issues exposed by `aiortc`, including fixed-width fingerprint parsing and multi-fingerprint SDP handling so the client now selects the advertised `sha-256` fingerprint correctly.
- 2026-04-18: Confirmed the remaining live-device failure is in the post-connect media path, not the control path: the board can still crash in inbound SRTP/RTP handling after the control channel is live.
- 2026-04-18: Disabled local firmware tones for quieter overnight testing and recorded the next implementation direction: port the known-good Pipecat Echo Pyramid media pipeline into the Hermes client while keeping Hermes-specific control semantics on top.
- 2026-04-18: Vendored the working `esp-libopus` component from the Pipecat reference tree, added a real Hermes media module, and wired the Pyramid to publish Opus mic audio and decode inbound Opus audio instead of stopping at the control channel.
- 2026-04-18: Hardened sender-side SRTP/libpeer behavior so the device no longer crashes as soon as the audio publisher starts; the Pyramid now reaches a stable live session with Flux attached on the gateway.
- 2026-04-18: Added gateway and firmware logging for live media/turn debugging. Current live state: gateway receives the device audio track, Flux connects, ICE completes, the data channel opens, and the Pyramid stays in `listening` awaiting a spoken turn.
- 2026-04-18: Fixed the missing `peer_init()` call so `libsrtp` initializes correctly on-device, verified continuous audio packet flow, and confirmed the first real spoken turn reaches the fake Hermes backend and plays back on the Pyramid speaker.
- 2026-04-18: Added a Hermes-home-native install path: `hermes-iot-setup`, `hermes-iot-doctor`, profile autodiscovery from `~/.hermes/hermes_iot.yaml` and `~/.hermes/hermes_iot.env`, recovery-to-listening on backend errors, and green verification with `10 passed` plus a clean setup/doctor smoke test.
- 2026-04-18: Turned the WebRTC simulator into an explicit Echo Pyramid / ESP32 client profile, added an in-process self-test path plus `make sim-esp32`, and verified the local no-hardware loop with a clean `1 passed` integration test and a live self-test run showing `listening -> thinking -> tool -> speaking -> idle -> listening`.
- 2026-04-18: Extended the simulator with reconnect, interrupt, mute, and volume scenarios, fixed interrupt recovery so cancelled turns resume to `listening`, and verified the expanded local loop with `14 passed` plus a live self-test showing stable session reuse across reconnects.
- 2026-04-23: Captured project intent and bring-up caveats for future development sessions.
- 2026-04-23: Revalidated local hardware after a reflash: the blank AtomS3R display is expected because `board_status.cpp` is stubbed, the board joins Wi-Fi, reaches the local gateway, completes WebRTC, and enters `listening`.
- 2026-04-23: Captured the current live-audio blocker: gateway receives the Pyramid audio track and Flux receives frames, but user speech did not emit a turn; device logs showed very low mic peaks.
- 2026-04-23: Downloaded official AtomS3R / Voice Pyramid schematics, datasheets, M5Echo-Pyramid source snippets, and the official ESPHome example into `docs/reference/vendor/`; added `docs/reference/hardware-lookup.md` with pin, I2C, codec, audio-format, and known-mismatch lookup tables.
- 2026-04-23: Fixed the live mic path to match M5's MIC1+MIC3 stereo capture pattern, sending slot0 (`mic`) upstream and preserving slot1 (`ref`) for diagnostics/future AEC-aware handling; verified the board now shows speech peaks in the thousands while the ref slot stays near idle.
- 2026-04-23: Fixed stale-peer disconnect handling in the gateway so a closed old peer no longer marks the reused device session disconnected after a clean reconnect; verified with `15 passed` in `gateway/tests`.
- 2026-04-23: Repaired local Hermes authentication for live testing, then verified a direct `/v1/responses` smoke test returned `completed`.
- 2026-04-23: Verified a full real hardware loop using `hello-this-is-drew.m4a`: Flux transcribed the sample, Hermes processed it and called the memory tool, the device received streamed text/state updates, entered `speaking`, then returned to `listening`.
- 2026-04-23: Reduced noisy live logging in the gateway and firmware, rebuilt and reflashed the Pyramid, then re-ran the sample loop successfully with readable logs: Flux turn, Hermes `200`, device `thinking -> speaking -> listening`.
- 2026-04-23: Fixed the fast/high-pitched TTS downlink risk by changing Aura/default downlink output to 16 kHz and making `PCMQueueAudioTrack` buffer, slice, and pace exact 20 ms frames; verified `gateway/tests` with `16 passed` and replayed the live sample through the patched gateway.
- 2026-04-23: Captured Deepgram Aura directly to WAV and captured the gateway's outbound aiortc audio through a simulated WebRTC receiver; Deepgram STT transcribed the gateway capture, proving the server-side TTS/WebRTC output is intelligible before ESP playback.
- 2026-04-23: Patched vendored libpeer RTP generic decode to honor CSRC count, RTP header extensions, and RTP padding before passing Opus payload bytes to `opus_decode`; rebuilt, flashed, and verified another live sample turn reaches `speaking -> listening`.
- 2026-04-23: Fixed physical downlink audio by matching the official M5Echo-Pyramid output path: Si5351 now configures CLK1 for 4.096 MHz MCLK using the vendor 16 kHz PLL/divider path, only CLK1 is enabled, ES8311 speaker output opens as stereo, and mono PCM is duplicated into left/right samples before I2S write. Rebuilt, reflashed, and verified by ear with `hello-this-is-drew.m4a` direct playback through the gateway debug endpoint.
- 2026-04-23: Fixed the stale TTS output-track bug on reconnect by forcing Aura output sessions to detach/reconnect when the same gateway session gets a new WebRTC output track; added assistant-echo transcript suppression, post-playback echo-window handling, and tests for the new heuristics.
- 2026-04-23: Measured the main live latency source: Hermes `/v1/responses` on the accumulated `iot:echo-pyramid-dev` conversation took about 15.6 seconds to first text, while a fresh conversation took about 2.0 seconds. The gateway now defaults to session-scoped Hermes conversations for live voice responsiveness, with `HERMES_IOT_CONVERSATION_MODE=device` available when long-lived device history is preferred.
- 2026-04-23: Added live turn timing logs and an Aura drain wait so first-token latency, Hermes completion time, and TTS drain time are visible in gateway logs. Latest controlled hardware test showed the first live response reaching first text in the 3-4 second range and playing on the Pyramid speaker.
- 2026-04-23: Reviewed the official M5 Xiaozhi Echo Pyramid guide, the public `78/xiaozhi-esp32` source, the M5Echo-Pyramid library, and the official ESPHome Voice Pyramid example. Captured the findings in `docs/reference/xiaozhi-review.md`: Xiaozhi is the best reference for full-duplex/AEC state, bounded audio queues, Opus framing, and local audio diagnostics, while Voice Pyramid hardware details must continue to come from M5Echo-Pyramid and ESPHome.
- 2026-04-23: Implemented the first Xiaozhi-inspired firmware/gateway pass: device hello now advertises audio reference/stats/loopback capabilities, firmware emits `audio.stats` with mic/ref/playback/send counters, `device.command` can trigger a local mic-to-speaker loopback diagnostic, and the gateway persists `audio.stats` in session device state. Verified with `25 passed` in gateway tests and a clean ESP-IDF 5.5 firmware build.
- 2026-04-24: Verified the loopback diagnostic on live hardware after reflashing: the gateway accepted `audio_loopback`, the device paused upstream/downstream media, captured local mic audio, played it through the Pyramid speaker path, and logged `Local audio loopback result=ESP_OK`. Committed and pushed this as `38e344e`.
- 2026-04-24: Added a device-side filter for aiortc/WebRTC idle Opus silence packets (`<=3` bytes) so the ESP no longer decodes/enqueues continuous server-side silence as active playback. The firmware now reports `remote_silence_packets` in `audio.stats` so this suppression remains observable. Verified with `25 passed`, a clean ESP-IDF 5.5 build, and `hermes-iot-sim --self-test`; live flash is still pending because no USB serial device was visible during this pass.
- 2026-04-24: Started the real Hermes-native gateway path. Added a native session manager that can route device speech turns through an in-process Hermes message handler, added a Hermes `IoTAdapter` wrapper that hosts the existing WebRTC/FastAPI transport inside Hermes, and added an installer path intended to patch a local Hermes checkout with `Platform.IOT`. This is WIP and not live-tested yet; focused compile plus `gateway/tests/test_session.py gateway/tests/test_installer.py` passed.
- 2026-04-24: Captured the hackathon north star and next UX/tool requirements before pausing live tests: win with an excellent home-running IoT assistant for moms, wake word is `hey willow`, the client should return to sleep after about 30 seconds of inactivity, and the default native IoT toolset should support get-time and named timers. Later skill loading should be intentional, not inherited from generic Hermes defaults.
- 2026-04-25: Implemented gateway-side wake/sleep gating: Flux can still hear the wake phrase, but sleeping sessions ignore ambient transcripts until `hey willow`; accepted activity extends a 30-second awake window and inactivity returns the device to `sleeping`. Added `iot_get_time` and `iot_set_timer` plugin tools, timer completion feedback through device commands, setup defaults for wake/sleep, and tests covering wake gating, timers, native installer toolsets, and the full gateway suite (`32 passed`).
- 2026-04-25: Ported the working Pipecat Echo Pyramid ESP-SR wake-word path into the Hermes firmware. Added `esp-sr`, the `wn9_heywillow_tts` model config, a `model` flash partition, and a WakeNet task that listens locally until `hey willow` before requesting WebRTC. Changed gateway defaults/profile install to leave server-side text wake gating off for the physical device path. Verified with a clean ESP-IDF 5.5 build and `PYTHONPATH=. pytest` (`32 passed`).
- 2026-04-25: Flashed the WakeNet firmware to `/dev/cu.usbmodem101` including `srmodels.bin`, then verified serial boot through Wi-Fi, gateway claim, `WebRTC armed; waiting for local wake word`, and `Wake word model=wn9_heywillow_tts words=Hey,Willow chunk_samples=512 rate=16000`. Added a firmware 30-second idle close path and reflashed it as well.
- 2026-04-25: Investigated a native IoT gateway behavior bug where the assistant entered irrelevant desktop/email workflows. Identified overly broad IoT tool/context inheritance as the issue, narrowed native IoT to `hermes-iot` + `no_mcp` with `skip_memory`, patched the live Hermes profile accordingly, then stopped the running gateway to avoid unattended speech/WebRTC usage.
