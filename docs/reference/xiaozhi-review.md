# Xiaozhi / M5 Voice Assistant Reference Review

This note records what is useful from the public Xiaozhi ESP32 tree and the
official M5Stack Voice Pyramid examples for the Hermes IoT gateway.

## Sources Reviewed

| Source | URL / local path | Notes |
| --- | --- | --- |
| M5 Voice Pyramid Xiaozhi guide | https://docs.m5stack.com/en/guide/realtime/xiaozhi/echo_pyramid | Proves AtomS3R + Voice Pyramid is an official voice-assistant target, but only documents M5Burner firmware flow. |
| Xiaozhi ESP32 source | https://github.com/78/xiaozhi-esp32 | Public source has M5Stack AtomS3R/Echo Base boards, not a first-class Voice Pyramid board directory at the time reviewed. |
| M5 Voice Pyramid product docs | https://docs.m5stack.com/en/atom/Echo_Pyramid | Hardware truth for Voice Pyramid: ES8311, ES7210, AW87559, Si5351, STM32 touch/RGB controller. |
| M5 Voice Pyramid Arduino tutorial | https://docs.m5stack.com/en/arduino/projects/atomic/echo_pyramid | Official M5Echo-Pyramid library path and begin/read/write behavior. |
| M5 Voice Pyramid ESPHome example | [vendor/m5stack-esphome/echo_pyramid_example.yaml](vendor/m5stack-esphome/echo_pyramid_example.yaml) | Official Home Assistant config for the AtomS3R + Voice Pyramid pairing. |
| M5Echo-Pyramid source snippets | [vendor/m5echo-pyramid/src](vendor/m5echo-pyramid/src) | Best local source of truth for Pyramid-specific Si5351, ES7210, ES8311, and I2S behavior. |

## What Xiaozhi Teaches Us

Xiaozhi is architecturally close to Hermes IoT, even if the cloud target is
different:

- It separates board support, audio service, protocol transport, and
  application state.
- It supports `kListeningModeRealtime`, but only defaults to that mode when AEC
  is enabled. Without AEC it falls back to auto-stop listening.
- It treats AEC as an audio-processing pipeline concern. The codec layer exposes
  input channels and optional reference input; the AFE layer decides whether to
  enable device-side AEC.
- It uses fixed audio queues and bounded encode/decode/playback queues rather
  than writing directly from network callbacks to speaker hardware.
- It uses Opus frames over a lightweight audio channel and negotiates
  `format=opus`, `sample_rate=16000`, `channels=1`, and frame duration in the
  hello/audio params.
- It has a simple audio test loop in Wi-Fi/config mode that records audio and
  plays it back locally. That is worth copying as a firmware diagnostic mode.

## M5Stack AtomS3R/Echo Base Board Details From Xiaozhi

The public Xiaozhi tree has `main/boards/atoms3r-echo-base`:

- I2C external bus: SDA `GPIO38`, SCL `GPIO39`.
- I2S: WS `GPIO6`, BCLK `GPIO8`, DIN `GPIO7`, DOUT `GPIO5`.
- Audio sample rate: `24000` for that Echo Base board.
- Audio codec: ES8311 only in the board file, with `AUDIO_INPUT_REFERENCE true`
  defined in config but not routed through `BoxAudioCodec`.
- Speaker mute uses a `PI4IOE` expander at `0x43`.
- Boot button toggles chat state.

This board is useful for AtomS3R patterns, display/UI behavior, and the
Xiaozhi state machine. It is not a drop-in hardware map for Voice Pyramid.

## Voice Pyramid Differences We Must Keep

Voice Pyramid-specific behavior should continue to come from M5Echo-Pyramid and
ESPHome, not from Xiaozhi Echo Base:

- I2C external bus remains SDA `GPIO38`, SCL `GPIO39`.
- I2S bus per ESPHome and our known-good firmware:
  - BCLK `GPIO6`
  - LRCK/WS `GPIO8`
  - speaker DOUT `GPIO7`
  - mic DIN `GPIO5`
- M5Echo-Pyramid initializes Si5351 first, then I2S, then ES7210, ES8311, STM32,
  and AW87559.
- Si5351 CLK1 must output `sample_rate * 256`; for 16 kHz this is `4.096 MHz`.
- M5Echo-Pyramid enables ES7210 `MIC1 | MIC3`.
- M5Echo-Pyramid reads stereo slots and labels slot 0 as `mic`, slot 1 as `ref`.
- M5Echo-Pyramid writes mono speaker PCM by duplicating it to stereo.
- ESPHome config models the speaker as mono through ES8311, but the lower-level
  working path still requires stereo duplication before I2S write in our
  firmware.
- Normal power should come through the Voice Pyramid base USB-C port, not only
  the AtomS3R USB-C port.

## AEC Interpretation

The M5 product page advertises ES7210 AEC/noise suppression/full-duplex support,
but the useful public code paths expose this as capture/reference plumbing plus
software/AFE behavior:

- Xiaozhi `BoxAudioCodec` can expose `input_reference`; when true it reads two
  channels so the audio processor can treat one channel as mic and one as
  reference.
- Xiaozhi `AfeAudioProcessor` builds an input format of `M...R` and enables
  Espressif AFE AEC only when `CONFIG_USE_DEVICE_AEC` is set.
- Xiaozhi explicitly warns that device-side AEC requires a clean output
  reference path and physical acoustic isolation.
- Xiaozhi server-side AEC uses packet timestamps and is marked unstable in its
  Kconfig help.

For Hermes IoT this means transcript echo filtering should stay as a guardrail,
not the main solution. The real next step is to either:

- add an ESP AFE processing stage on-device using the Pyramid `mic/ref` slots,
  then send processed mono upstream, or
- send mic plus reference/timestamps to the gateway/provider path if we want
  server-side/provider-side AEC.

## Actionable Deltas For Hermes IoT

- Add a local audio test mode on the firmware that records a short mic buffer
  and plays it back through the speaker, similar to Xiaozhi's config-mode audio
  testing.
- Keep full-duplex mode, but treat it as "requires AEC path enabled." If AEC is
  not enabled, the gateway can still run full transport but should expect more
  transcript echo suppression.
- Add an `audio_reference` capability to the device hello once we expose the
  Pyramid ref slot intentionally.
- Add device telemetry for mic/ref peaks and playback queue depth over the
  control channel so gateway logs can distinguish acoustic echo from network
  jitter.
- Consider an ESP AFE dependency experiment in firmware behind Kconfig, not in
  the default path until it builds and fits memory reliably.
- Keep the current M5Echo-Pyramid Si5351/stereo-write baseline pinned; it is the
  known-good fix for intelligible speaker output.

## Current Decision

Do not port Xiaozhi wholesale. Use it as a design reference for full-duplex
state, AEC gating, audio queueing, Opus framing, and device diagnostics. Keep
the Hermes protocol, WebRTC transport, and Hermes `/v1/responses` runtime.
Keep Voice Pyramid hardware behavior aligned with M5Echo-Pyramid plus ESPHome.
