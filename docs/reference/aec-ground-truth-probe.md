# Echo Pyramid AEC Ground Truth Probe

This probe intentionally steps away from the voice-agent loop. Its job is to
answer whether the device can play a known local signal and capture the expected
MIC1 voice slot, MIC3 DAC/AEC reference slot, and ESP-SR AEC output.

## Run

Wake the device so it connects to the gateway, then run:

```bash
curl -s \
  -H 'X-Admin-Key: dev-admin-key' \
  -H 'Content-Type: application/json' \
  -d '{"type":"audio.aec_probe","payload":{"duration_ms":5000,"frequency_hz":0,"amplitude":3500}}' \
  http://127.0.0.1:8787/v1/devices/echo-pyramid-dev/commands | jq .
```

Set `frequency_hz` to `0` for deterministic broadband noise. Use a nonzero
frequency for a sine, but do not rely on sine-only results for AEC quality;
adaptive echo cancellation needs broadband content to converge.

Then inspect:

```bash
curl -s \
  -H 'X-Admin-Key: dev-admin-key' \
  http://127.0.0.1:8787/v1/devices/echo-pyramid-dev | jq .device_state.audio_aec_probe
```

The firmware also logs a single `AEC_PROBE` line on serial.

## Reading Results

Useful fields:

- `ref_peak` / `ref_rms`: proves whether the ES8311 DAC analog reference is
  arriving through ES7210 MIC3.
- `mic_peak` / `mic_rms`: acoustic speaker leakage into MIC1.
- `aec_peak` / `aec_rms`: ESP-SR output after cancellation.
- `slot_rms` / `slot_corr`: per-slot TDM diagnostics. Slot 0 should be the
  voice mic; the strongest playback-reference slot should show clear correlation
  with slot 0 during the probe.
- `mic_ref_corr`: how strongly raw MIC1 correlates with the reference.
- `aec_ref_corr`: how strongly the AEC output still correlates with the
  reference.
- `suppression_db`: positive means AEC output RMS is lower than raw MIC RMS;
  negative means AEC made it louder.

Expected direction if the hardware reference and AEC path are healthy:

```text
ref_rms should be clearly above idle noise.
aec_rms should be lower than mic_rms.
abs(aec_ref_corr) should be lower than abs(mic_ref_corr).
suppression_db should be positive.
```

If `ref_rms` is near idle noise during playback, the issue is still in the
ES7210 TDM/reference capture path. If `ref_rms` is strong but `suppression_db`
is negative, the reference is probably misaligned, inverted, over/under gained,
or passed into ESP-SR AEC with the wrong framing.

## Known Good Result

After porting the standalone AEC lab settings into the main firmware, the boot
probe produced:

```text
AEC_PROBE result=ESP_OK signal=noise frames=307
mic_peak=1168 ref_peak=434 aec_peak=91
mic_rms=228.6 ref_rms=98.2 aec_rms=14.8
mic_ref_corr=0.020 aec_ref_corr=0.001
suppression_db=23.77
slot_rms=[228.6,98.2,0.0,0.0]
```

The important readback logs near that probe were:

```text
ESP-SR AEC enabled mode=VOIP_HIGH_PERF frame_samples=256 filter_length=4
ES7210 TDM reference ready clock=0x00 fmt=0x60 tdm=0x02 mic1=0x18 mic3=0x1e
AW87559 reg[0x01] = 0x78 reg[0x05] = 0x10
```

For normal demo firmware, `CONFIG_HERMES_IOT_BOOT_AEC_PROBE` should be off so
the device does not play the broadband probe at boot. Keep the command-triggered
probe available for diagnostics.
