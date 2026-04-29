# Echo Pyramid AEC Lab

This is a standalone ESP-IDF firmware target at `firmware/aec-lab`. It is
intentionally separate from the Hermes voice client so AEC can be proven without
WebRTC, WakeNet, Deepgram, TTS, or gateway state.

## Current Ground Truth

The lab proved the Echo Pyramid production hardware has a real ES8311-to-ES7210
analog AEC reference path.

Working capture/playback shape:

- ES8311 playback stays normal slave I2S.
- ES7210 capture uses 4-slot TDM on the wired ESP RX pin.
- TDM slot 0 is MIC1 voice.
- TDM slot 1 is MIC3, the ES8311 DAC/AEC reference.
- ESP-SR AEC works best with `AEC_MODE_VOIP_HIGH_PERF` and filter length `4`.

Working board-level settings from the lab:

```text
I2S bus rate:       48 kHz in the lab, 16 kHz in main firmware
ES7210 0x08:        0x20
ES7210 0x11:        0x60
ES7210 0x12:        0x02
ES7210 0x43 MIC1:   0x18
ES7210 0x45 MIC3:   0x1e
AW87559 0x01:       0x78
AW87559 0x05:       0x10
ESP-SR AEC mode:    AEC_MODE_VOIP_HIGH_PERF
ESP-SR filter len:  4
```

Best standalone lab result:

```text
LAB_RESULT name=tdm_sys78_aec4_voip_high result=ESP_OK
slot_rms=[296.6,122.8,0.0,0.2]
aec_rms=6.9
suppression_db=32.62
```

Main firmware boot probe after porting these settings:

```text
AEC_PROBE result=ESP_OK signal=noise
mic_rms=228.6 ref_rms=98.2 aec_rms=14.8
suppression_db=23.77
slot_rms=[228.6,98.2,0.0,0.0]
```

This is the baseline to preserve. If self-interruption regresses again, first
verify these register readbacks and the `ESP-SR AEC enabled
mode=VOIP_HIGH_PERF frame_samples=256 filter_length=4` log before changing
gateway or Deepgram behavior.

## Run

Build and flash:

```bash
source ~/esp/esp-idf/export.sh
idf.py -C firmware/aec-lab set-target esp32s3
idf.py -C firmware/aec-lab -p /dev/cu.usbmodem1101 flash monitor
```

The useful serial lines are `LAB_CASE_BEGIN` and `LAB_RESULT`.

The lab currently tests:

- Amp-muted TDM reference capture to prove MIC3 receives the DAC tap without
  acoustic speaker leakage.
- AW87559 enabled with ESPHome/M5-compatible `0x01 = 0x78`.
- ESP-SR AEC mode/filter sweeps around the same hardware setup.

Interpretation:

- A valid analog reference path should make one non-MIC slot show clear
  playback energy during the deterministic broadband playback probe.
- If MIC1 is high and MIC3 is low across all profiles, the issue is before AEC:
  the reference arriving from ES7210 is too weak or not the intended signal.
- Only after a strong reference is visible should `suppression_db` be used as a
  meaningful AEC quality signal.
- If `AEC_MODE_SR_LOW_COST` shows only modest suppression, do not assume the
  hardware path is wrong. On this device `AEC_MODE_VOIP_HIGH_PERF` was the first
  mode that produced strong cancellation.
