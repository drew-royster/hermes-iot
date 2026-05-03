# M5Stack ESPHome Reference

This directory stores local reference copies of M5Stack's ESPHome support for
the AtomS3R + Voice Pyramid / Echo Pyramid pairing.

## Snapshot

| Field | Value |
| --- | --- |
| Upstream | https://github.com/m5stack/esphome-yaml |
| Local snapshot | [esphome-yaml](esphome-yaml) |
| Commit | `9dcd5199435d1c6d65c43af3bb99b7d999ca7e54` |
| Retrieved | 2026-05-03 |

The snapshot is vendored without its `.git` directory so this repo can track
the reference files directly.

## Echo Pyramid Files

| File | Why it matters |
| --- | --- |
| [esphome-yaml/examples/voice_assistant/echo_pyramid_example.yaml](esphome-yaml/examples/voice_assistant/echo_pyramid_example.yaml) | Full ESPHome configuration for AtomS3R + Voice Pyramid, including pins, audio devices, display, touch, RGB, and voice assistant state handling. |
| [esphome-yaml/components/si5351/si5351_esphome.cpp](esphome-yaml/components/si5351/si5351_esphome.cpp) | M5Stack's ESPHome Si5351 setup for the Pyramid clock generator. Useful when checking MCLK register values. |
| [esphome-yaml/components/aw87559/aw87559_esphome.cpp](esphome-yaml/components/aw87559/aw87559_esphome.cpp) | Speaker amplifier setup. It writes `0xff` to chip ID register `0x00` and `0x78` to sysctrl register `0x01`. |
| [esphome-yaml/components/pyramidtouch/pyramidtouch.h](esphome-yaml/components/pyramidtouch/pyramidtouch.h) | STM32 touch controller status registers for touch points 1-4. |
| [esphome-yaml/components/pyramidrgb/pyramidrgb.h](esphome-yaml/components/pyramidrgb/pyramidrgb.h) | STM32 RGB controller brightness and color register addresses for the four LED channels. |

The older single-file copy at [echo_pyramid_example.yaml](echo_pyramid_example.yaml)
is preserved for compatibility with previous notes.
