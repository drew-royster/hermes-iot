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

2026-05-03 production firmware boot probe after updating to ESP-IDF `v5.5.4`,
`esp-sr 2.4.3`, `esp_codec_dev 1.5.9`, and forcing the main app onto the
measured-good 4-slot TDM interface:

```text
ESP-IDF: v5.5.4
ES7210 MIC1/MIC3 reference ready clock=0x00 fmt=0x60 if2=0x02 mic1=0x18 mic3=0x1e
I2S stream mode ready: TX/RX 4-slot TDM 16-bit slots
ESP-SR AEC enabled mode=VOIP_HIGH_PERF frame_samples=256 filter_length=4
ES7210 reg[0x12] = 0x02
AEC_PROBE result=ESP_OK signal=noise frames=307
mic_rms=239.9 ref_rms=98.2 aec_rms=53.6
suppression_db=13.02
slot_rms=[239.9,98.2,0.0,0.0]
```

2026-05-03 truth-table rerun on the same production board, using deterministic
broadband playback and the same MIC1/MIC3 gain targets:

```text
LAB_RESULT name=std_vendor_ref_amp_off result=ESP_OK
slot_rms=[11.9,0.1,0.0,0.0] suppression_db=1.29

LAB_RESULT name=std_vendor_sys78_voip_high result=ESP_OK
slot_rms=[235.7,0.0,0.0,0.0] suppression_db=1.24

LAB_RESULT name=std_reg12_02_sys78_voip_high result=ESP_OK
slot_rms=[235.6,0.1,0.0,0.0] suppression_db=1.24

LAB_RESULT name=tdm_sys78_aec4_voip_high result=ESP_OK
slot_rms=[236.7,122.6,0.0,0.2] suppression_db=30.19
```

2026-05-03 latest-stack rerun after updating to ESP-IDF `v5.5.4`,
`esp-sr 2.4.3`, and the 2026 toolchain:

```text
LAB_RESULT name=std_vendor_ref_amp_off result=ESP_OK
slot_rms=[11.9,0.0,0.0,0.0] suppression_db=2.11

LAB_RESULT name=std_vendor_sys78_voip_high result=ESP_OK
slot_rms=[240.9,0.0,0.0,0.0] suppression_db=1.22

LAB_RESULT name=std_reg12_02_sys78_voip_high result=ESP_OK
slot_rms=[240.4,0.1,0.0,0.0] suppression_db=1.23

LAB_RESULT name=tdm_ref_amp_off result=ESP_OK
slot_rms=[40.1,123.7,0.1,0.2] suppression_db=-0.72

LAB_RESULT name=tdm_sys78_aec4_sr_low result=ESP_OK
slot_rms=[241.4,122.7,0.0,0.2] suppression_db=12.74

LAB_RESULT name=tdm_sys78_aec8_sr_low result=ESP_OK
slot_rms=[240.2,122.6,0.1,0.2] suppression_db=14.21

LAB_RESULT name=tdm_sys78_aec12_sr_low result=ESP_OK
slot_rms=[237.9,122.1,0.0,0.2] suppression_db=-36.78

LAB_RESULT name=tdm_sys78_aec4_sr_high result=ESP_OK
slot_rms=[240.3,122.6,0.0,0.2] suppression_db=13.29

LAB_RESULT name=tdm_sys78_aec8_sr_high result=ESP_OK
slot_rms=[239.1,122.8,0.0,0.2] suppression_db=14.61

LAB_RESULT name=tdm_sys78_aec4_voip_high result=ESP_OK
slot_rms=[243.5,122.8,0.0,0.2] suppression_db=31.00
```

This does not disprove the vendor docs. It proves the physical MIC3 reference
is present, while standard stereo RX did not expose it in either the old or
latest IDF lab path. For the Hermes firmware, treat 4-slot TDM RX as the
measured-good reference capture mode until a standard-stereo implementation
produces non-idle slot 1 energy under this same probe.

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

- Vendor-shaped standard stereo RX cases with ES7210 MIC1/MIC3 gain targets.
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
