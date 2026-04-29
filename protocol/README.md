# Hermes IoT control protocol v1

The v1 contract splits responsibilities:

- WebRTC media tracks carry upstream microphone audio and downstream assistant audio.
- A reliable ordered data channel carries device control and assistant state.

## Required message types

- `hello`
- `device.state`
- `assistant.state`
- `assistant.text.delta`
- `tool.progress`
- `audio.input.level`
- `audio.stats`
- `debug.user_text`
- `interrupt`
- `mute.set`
- `volume.set`
- `device.command`
- `error`

## Handshake

1. Device pairs via `POST /v1/pair/claim`.
2. Device authenticates signaling with the returned bearer token.
3. Device sends a WebRTC offer to `POST /v1/devices/{device_id}/webrtc/offer`.
4. After the peer connection is established, the device opens a data channel and sends a `hello` message.
5. The gateway answers with session metadata and transitions into listening mode once the upstream audio track is attached.

## Full-duplex flow

1. Device streams microphone audio continuously over the WebRTC upstream track.
2. Flux performs conversational STT and end-of-turn detection on that stream.
3. On `EndOfTurn`, the gateway sends the recognized turn to Hermes.
4. Assistant deltas are forwarded to Aura streaming TTS and emitted on the downstream audio track.
5. If Flux signals a resumed/new user turn while the assistant is speaking, the gateway interrupts the active assistant turn and returns to listening.

## Debug text mode

`debug.user_text` exists only for local bring-up and automated tests so the Hermes loop can be exercised without live STT. Real devices should not depend on it.

## Assistant states

- `idle`
- `listening`
- `thinking`
- `tool`
- `speaking`
- `error`

The gateway owns these transitions. Devices render them in their native UI.

## Audio ingest bring-up

The default backend now consumes upstream audio tracks and can emit `audio.input.level` messages over the data channel as a keepalive/bring-up signal. This is not a final UX message type; it exists so firmware and simulator clients can validate that the media path is live before STT is fully integrated.

## Audio diagnostics

Devices may emit `audio.stats` messages with hardware-level capture/playback counters such as `mic_peak`, `ref_peak`, `playback_underruns`, `playback_overflows`, `remote_silence_packets`, and encoded packet counters. This mirrors the useful Xiaozhi-style diagnostics path: keep the live gateway observable enough to distinguish acoustic echo, capture problems, and network/playback jitter without rerunning the full assistant pipeline.
