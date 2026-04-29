# Echo Pyramid AEC Debug Notes

Date: 2026-04-27

## Goal

Make the AtomS3R + M5Stack Echo Pyramid behave like a full-duplex assistant:
the microphone must stay live during assistant speech for barge-in, while AEC
prevents the device from transcribing its own speaker output.

## Known Hardware Context

- Echo Pyramid uses ES8311 DAC/output, ES7210 ADC/input, AW87559 speaker amp,
  and Si5351 MCLK.
- M5 reference code initializes ES7210 with MIC1 and MIC3.
- esphome-intercom documents an ES7210 TDM hardware-reference pattern:
  MIC1 as voice, MIC3 as DAC reference, TDM slot 0 as mic, TDM slot 1 as ref.

## Experiments Tried

### Baseline stereo ES7210 capture

- ES7210 opened as normal stereo with MIC1 + MIC3.
- Playback quality was good.
- Observed reference channel stayed near idle during playback.
- Gateway/device stats showed `ref_peak` around `2..8`.

Conclusion: normal stereo capture did not expose a usable playback reference.

### Full TDM TX + RX

- Configured both I2S TX and RX as 4-slot TDM.
- ES7210 opened with MIC1..MIC4 and TDM mode.
- Mic capture stayed alive.
- Playback debug tone decoded and wrote frames.
- Observed reference slot still stayed near idle during playback:
  `slots=[...,6..8,...]`.

Conclusion: all-TDM did not route speaker output into the ES7210 reference slot
on this firmware path.

### Direct I2S TDM speaker write

- Replaced `esp_codec_dev_write` with direct `i2s_channel_write` for TDM TX.
- Wrote mono speaker samples to TDM slot 0, matching esphome-intercom's direct
  write pattern.
- Playback still decoded/wrote, but ES7210 ref slot stayed near idle.

Conclusion: the missing reference was not caused only by `esp_codec_dev_write`.

### Hybrid TX stereo + RX TDM

- Restored ES8311 playback to normal stereo TX.
- Kept ES7210 capture as 4-slot TDM RX.
- Mic and WakeNet worked.
- Playback was audible, and the mic acoustically heard the speaker enough to
  trigger transcript activity.
- ES7210 ref slot still stayed near idle during playback.

Conclusion: the device still hears itself acoustically, but no captured slot is
currently carrying the desired electrical playback reference.

### ES7210 register patch from esphome-intercom

Applied safe parts of the external reference:

- Clear ES7210 reg `0x01` bits `0..5` to enable ADC clocks.
- Set ES7210 reg `0x12 = 0x02` for TDM mode.
- Set ES7210 MIC3 gain reg `0x45 = 0x1A`.
- Set unused MIC4 gain reg `0x46 = 0x10`.

Observed diagnostics after patch:

- `0x01 = 0x00`
- `0x12 = 0x02`
- `0x45 = 0x1a`
- `0x46 = 0x10`
- mic recovered and WakeNet worked
- ref slot still stayed near idle during playback

Conclusion: these register writes are safe but did not surface the playback ref.

### ES7210 0x4B/0x4C power-switch write

- Tried setting `0x4B = 0x0F` and `0x4C = 0x0F`, matching the M5 ES7210 source.
- Result: all capture slots dropped to `1`, and WakeNet could not hear.

Conclusion: do not use those writes blindly with the current `esp_codec_dev`
init path.

### TDM hardware-reference implementation from GPT-5.5 Pro handoff

Implemented the architecture described in the 2026-04-27 research handoff:

- Si5351 remains at 48 kHz bus / 12.288 MHz MCLK.
- ES8311 remains normal slave I2S/Philips playback at the codec-register level.
- ES7210 is opened in 4-slot TDM mode.
- Runtime playback initially wrote four-slot TDM frames as `L/R/0/0`.
- Follow-up correction: TX now uses standard Philips stereo with 16-bit samples
  in 32-bit slots. This keeps ES8311 on normal stereo I2S timing while preserving
  a 64fs BCLK for ES7210 TDM RX.
- Runtime capture reads four-slot TDM frames directly from I2S.
- AEC consumes downsampled slot 0 as MIC1 voice and slot 1 as MIC3 DAC/AEC
  reference.
- MIC gain was reduced from 37.5 dB to 30 dB to avoid an over-hot far-field
  capture path while testing full-duplex cancellation.

Observed boot diagnostics after flashing:

- `ES7210 TDM reference ready clock=0x00 fmt=0x60 tdm=0x02 mic1=0x1a mic3=0x1a`
- Final I2S mode logs `TX standard stereo 16-bit/32-slot, RX TDM 4x16-bit`.
- Baseline idle capture stayed quiet on slot 1: examples `slots=[26,6,1,2]`,
  `slots=[40,7,1,1]`.

Remaining validation: run a playback-correlation test after WebRTC is open. The
gateway command endpoint cannot inject a beep while the device is intentionally
idle in local WakeNet mode because no data channel exists yet.

## Current Flashed State

The currently flashed build has:

- ES8311 normal I2S playback at the codec level.
- ESP TX as standard Philips stereo with 16-bit samples in 32-bit slots.
- ES7210 4-slot TDM RX with expected slot order MIC1, MIC3 reference, MIC2,
  MIC4.
- Direct raw I2S runtime I/O instead of `esp_codec_dev_read` /
  `esp_codec_dev_write`.
- ESP-SR AEC using slot 0 as mic and slot 1 as reference.
- No software playback-reference fallback.

It boots, connects, initializes AEC, and waits for local WakeNet. Hardware
reference correlation during playback still needs an active-session test.

## Rejected Shortcut

A software playback-reference FIFO was considered briefly:

- Downsample outgoing 48 kHz speaker PCM to 16 kHz.
- Use that FIFO as the ESP-SR AEC reference when playback is present.
- Keep mic live during speech.

This is intentionally not implemented. The goal is to make the Echo Pyramid work
the way the hardware/reference designs intend, not to paper over an incorrect
codec or slot configuration.

## Open Questions

- Does Echo Pyramid actually route ES8311 DAC analog output to ES7210 MIC3 on
  production hardware, or is the advertised AEC implemented another way?
- Does slot 1 rise and correlate during real playback now that TX also writes
  four-slot TDM frames?
- Does ES8311 standard playback tolerate the four-slot TDM ESP bus at all music
  volumes without artifacts?
- If hardware ref is not available, what delay/alignment is needed for a robust
  software playback-reference AEC path?
