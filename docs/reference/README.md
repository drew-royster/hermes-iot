# Hardware Reference Cache

This directory keeps the hardware facts and source artifacts used while bringing
up the Hermes IoT Echo Pyramid firmware.

The most useful file is [hardware-lookup.md](hardware-lookup.md). It compresses
the official AtomS3R and Voice Pyramid documentation into the values we keep
needing during firmware work: pins, I2C addresses, codec roles, audio format,
power notes, and current code mismatches.

Downloaded vendor artifacts live under [vendor/](vendor/). They are reference
copies from M5Stack docs and GitHub sources so board bring-up can continue even
when the web docs are not open.

## Source Groups

| Group | Local path | Purpose |
| --- | --- | --- |
| Official PDFs | [vendor/official](vendor/official) | Product PDFs, schematics, and component datasheets. |
| M5Echo-Pyramid library | [vendor/m5echo-pyramid](vendor/m5echo-pyramid) | Official Arduino/ESP-IDF-capable library source snippets and examples. |
| M5Stack ESPHome reference | [vendor/m5stack-esphome](vendor/m5stack-esphome) | Official Home Assistant / ESPHome YAML plus a vendored `m5stack/esphome-yaml` snapshot with Pyramid-specific components. |
| Xiaozhi reference review | [xiaozhi-review.md](xiaozhi-review.md) | Notes from reviewing M5/Xiaozhi examples for audio state, AEC, queueing, and firmware diagnostics. |

## Primary URLs

| Source | URL |
| --- | --- |
| Voice Pyramid product docs | https://docs.m5stack.com/en/atom/Echo_Pyramid |
| AtomS3R product docs | https://docs.m5stack.com/en/core/AtomS3R |
| Voice Pyramid Arduino tutorial | https://docs.m5stack.com/en/arduino/projects/atomic/echo_pyramid |
| Voice Pyramid Home Assistant tutorial | https://docs.m5stack.com/en/homeassistant/voice_assistant/echo_pyramid |
| M5Echo-Pyramid library | https://github.com/m5stack/M5Echo-Pyramid |
| M5Stack ESPHome components/examples | https://github.com/m5stack/esphome-yaml |
| Xiaozhi ESP32 source | https://github.com/78/xiaozhi-esp32 |
