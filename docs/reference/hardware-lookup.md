# AtomS3R + Voice Pyramid Hardware Lookup

This is the working lookup sheet for the Hermes IoT proof of concept. It favors
firmware-relevant facts over a full product summary.

M5Stack docs use `Voice Pyramid` for the product page and the project often says
`Echo Pyramid`. For this repo they refer to the same base, SKU `A167`, paired
with an `AtomS3R`.

## High-Confidence Hardware Facts

| Item | Value | Source |
| --- | --- | --- |
| Controller | AtomS3R, ESP32-S3-PICO-1-N8R8 | AtomS3R product docs |
| Flash / PSRAM | 8 MB flash, 8 MB PSRAM | AtomS3R product docs |
| AtomS3R display | 0.85 inch IPS LCD, 128 x 128, GC9107 | AtomS3R product docs |
| Voice base MCU | STM32G030F6P6 | Voice Pyramid product docs |
| Audio DAC/codec | ES8311 | Voice Pyramid product docs and schematic |
| Audio ADC / mic front end | ES7210 | Voice Pyramid product docs and schematic |
| Speaker amplifier | AW87559 | Voice Pyramid product docs and schematic |
| Audio MCLK source | Si5351, CLK1 to ES7210/ES8311 MCLK | Voice Pyramid product docs and schematic |
| Touch / RGB controller | STM32G030F6P6 controls touch and RGB LEDs | Voice Pyramid product docs |
| RGB LEDs | 28 WS2812 LEDs, two groups of 14 | Voice Pyramid product docs |
| Touch zones | 4 touch points, TP1-TP4 | Voice Pyramid product docs |

## Power And Seating Rules

| Rule | Why it matters |
| --- | --- |
| The AtomS3R must be fully seated on the Voice Pyramid base. | Without the base connection, the Pyramid I2C/audio devices will not appear. |
| For normal operation, power through the USB-C port on the Voice Pyramid base, not the AtomS3R. | M5Stack explicitly warns that powering only the AtomS3R can cause insufficient power for the base. |
| The AtomS3R USB-C port is enough for flashing. | M5Stack's Arduino tutorial says the AtomS3R USB connection is for firmware flashing. |
| A blank AtomS3R screen is not proof of failure in this repo. | Our display/status implementation is still stubbed. Use serial logs. |

## Buses And Pins

| Function | GPIO / bus | Notes |
| --- | --- | --- |
| Voice Pyramid external I2C SDA | GPIO38 | Official ESPHome example calls this `ext_bus`. |
| Voice Pyramid external I2C SCL | GPIO39 | Official ESPHome example calls this `ext_bus`. |
| AtomS3R board I2C SDA | GPIO45 | Used by AtomS3R onboard devices/backlight path. |
| AtomS3R board I2C SCL | GPIO0 | Used by AtomS3R onboard devices/backlight path. |
| I2S BCLK/SCLK | GPIO6 | Shared by ES8311 and ES7210. |
| I2S LRCK/WS | GPIO8 | Shared by ES8311 and ES7210. |
| I2S DIN / mic from ES7210 | GPIO5 | ES7210 `ASDOUT` into AtomS3R. |
| I2S DOUT / speaker to ES8311 | GPIO7 | AtomS3R audio output to ES8311 `DSDIN`. |
| Display SPI SCLK | GPIO15 | AtomS3R display. |
| Display SPI MOSI | GPIO21 | AtomS3R display. |
| Display DC | GPIO42 | AtomS3R display. |
| Display reset | GPIO48 | AtomS3R display. |
| Display CS | GPIO14 | AtomS3R display. |
| Button | GPIO41 | AtomS3R user button in ESPHome example. |

## Expected I2C Devices

These are the Voice Pyramid devices the official docs identify on the external
I2C bus, and they match the seated-board scan we observed except for the old
optional `0x43` probe in our code.

| Address | Device | Role |
| --- | --- | --- |
| `0x18` | ES8311 | Speaker DAC / codec. |
| `0x1A` | STM32G030F6P6 | Touch and RGB LED controller. |
| `0x40` | ES7210 | Microphone ADC / front end. |
| `0x5B` | AW87559 | Speaker amplifier. |
| `0x60` | Si5351 | Programmable MCLK generator. |

Current repo note: [board_audio.cpp](../../firmware/echo-pyramid/main/board_audio.cpp)
still probes `0x43` as `io_expander_addr`. The official Voice Pyramid docs do
not list that device; speaker reset is documented as AW87559 `SPK_RST` driven by
STM32 GPIOB7. Treat the `0x43` warning as non-blocking until we prove otherwise.

## Audio Format Baseline

| Path | Official baseline |
| --- | --- |
| Sample rate | 16000 Hz for ES8311/ES7210 in the official ESPHome example. |
| Bit depth | 16-bit. |
| Microphone I2S input | `GPIO5`, external ADC, stereo in the official ESPHome example. |
| Speaker I2S output | `GPIO7`, external DAC, mono in the official ESPHome example. |
| MCLK | `sample_rate * 256`; M5Echo-Pyramid library configures Si5351 MCLK before codecs. |
| ES7210 mic selection | Official M5Echo-Pyramid library initializes MIC1 and MIC3. |

## Known-Good Repo Audio Baseline

The current live-tested firmware matches the important official M5Echo-Pyramid
audio behavior:

| Area | Required behavior |
| --- | --- |
| Si5351 MCLK | Configure CLK1 for `4096000 Hz` when sample rate is `16000 Hz`. |
| Si5351 outputs | Enable only CLK1 with output-enable register `0xFD`; keep CLK0/CLK2 powered down. |
| CLK1 control | Use vendor CLK1 control byte `0x4F`. |
| ES8311 speaker stream | Open output as 16-bit stereo at 16 kHz. |
| Speaker PCM layout | Duplicate mono assistant samples into left/right I2S slots before write. |
| ES7210 mic stream | Open input as 16-bit stereo at 16 kHz with MIC1 and MIC3 enabled. |
| STT input | Send slot 0 (`mic`) upstream; keep slot 1 (`ref`) for diagnostics/future AEC-aware handling. |

This baseline fixed the high-pitched/garbled physical speaker output during live
debug playback. Do not regress it while tuning WebRTC, Deepgram, or Hermes.

## Wake Word Baseline

The physical Echo Pyramid path uses local ESP-SR WakeNet detection. This matches
the known-good Pipecat reference in `/Users/drewroyster/Documents/echo` and keeps
ambient asleep audio off the network.

| Area | Required behavior |
| --- | --- |
| Wake phrase | `hey willow`. |
| ESP-SR model | `wn9_heywillow_tts`. |
| Model filter string | `heywillow`. |
| Model storage | `model` partition at `0x210000`; ESP-IDF build emits `build/srmodels/srmodels.bin`. |
| Detection mode | `DET_MODE_90`, copied from the working Pipecat reference. |
| Runtime behavior | Wake task reads local mic frames while WebRTC is inactive; on detection it plays the wake tone and requests WebRTC. |
| Gateway behavior | Server-side transcript wake gating is disabled by default for the physical device path. |

## AEC Interpretation

The product page describes the ES8311 + ES7210 audio system as supporting AEC,
noise suppression, far-field capture, and full-duplex interaction. The ES7210
datasheet itself presents the chip as a high-performance four-channel audio ADC,
not as a standalone echo-cancellation DSP.

The strongest implementation clue is the official M5Echo-Pyramid library:
`M5EchoPyramid::read(int16_t *mic, int16_t *ref, int frames)` reads two I2S slots
and labels them `mic` and `ref`. The official examples record/play back `mic`
and ignore `ref`.

Repo policy for now:

| Slot | Meaning | Current use |
| --- | --- | --- |
| I2S slot 0 | `mic`, per M5 library | Sent upstream to Opus/STT. |
| I2S slot 1 | `ref`, per M5 library | Logged for diagnostics; reserved for future AEC-aware processing. |

Do not mix, average, or choose the louder slot as STT input. That can accidentally
send the reference/echo channel instead of the microphone channel.

## Resolved Code Mismatches

| Area | Resolution | Why it matters |
| --- | --- | --- |
| ES7210 input channels | Firmware now enables `ES7120_SEL_MIC1 | ES7120_SEL_MIC3`. | Matches the official two-mic capture path. |
| Mic codec open channel count | Firmware now opens mic capture as stereo and logs slot peaks. | Preserves the M5 `mic/ref` slot layout. |
| STT channel selection | Firmware sends slot 0 upstream and keeps slot 1 as diagnostics/ref. | Avoids accidentally sending the reference channel to STT. |
| Si5351 MCLK | Firmware now computes the vendor 16 kHz PLL/divider path and enables only CLK1. | Fixes physical playback pitch/quality by supplying correct codec MCLK. |
| Speaker I2S layout | Firmware now opens speaker output as stereo and duplicates mono PCM before write. | Matches the official M5 `write()` implementation for ES8311 output. |

## Remaining Repo-Specific Notes

| Area | Current repo behavior | Reference behavior | Why it matters |
| --- | --- | --- | --- |
| `0x43` IO expander | Repo probes `0x43` for speaker enable | Official docs list STM32 `0x1A`, not `0x43`, for speaker reset control. | This warning should not distract from audio capture or playback debugging. |
| Display | Stub only | AtomS3R docs define GC9107 display pins. | Blank screen is expected until `board_status.cpp` is implemented. |

## Local Reference Files

| Artifact | Local path |
| --- | --- |
| Voice Pyramid product PDF | [vendor/official/voice-pyramid-product.pdf](vendor/official/voice-pyramid-product.pdf) |
| Voice Pyramid schematic | [vendor/official/voice-pyramid-schematic.pdf](vendor/official/voice-pyramid-schematic.pdf) |
| AtomS3R product PDF | [vendor/official/atom-s3r-product.pdf](vendor/official/atom-s3r-product.pdf) |
| AtomS3R schematic | [vendor/official/atom-s3r-schematic.pdf](vendor/official/atom-s3r-schematic.pdf) |
| ES7210 datasheet | [vendor/official/es7210-datasheet.pdf](vendor/official/es7210-datasheet.pdf) |
| ES8311 datasheet | [vendor/official/es8311-datasheet.pdf](vendor/official/es8311-datasheet.pdf) |
| AW87559 datasheet | [vendor/official/aw87559-datasheet.pdf](vendor/official/aw87559-datasheet.pdf) |
| Si5351 datasheet | [vendor/official/si5351-b-datasheet.pdf](vendor/official/si5351-b-datasheet.pdf) |
| STM32G030C6 datasheet | [vendor/official/stm32g030c6-datasheet.pdf](vendor/official/stm32g030c6-datasheet.pdf) |
| Official ESPHome Echo Pyramid YAML | [vendor/m5stack-esphome/echo_pyramid_example.yaml](vendor/m5stack-esphome/echo_pyramid_example.yaml) |
| M5Stack ESPHome source snapshot | [vendor/m5stack-esphome/esphome-yaml](vendor/m5stack-esphome/esphome-yaml) |
| M5Stack ESPHome Pyramid components | [vendor/m5stack-esphome/README.md](vendor/m5stack-esphome/README.md) |
| M5Echo-Pyramid library snippets | [vendor/m5echo-pyramid/src](vendor/m5echo-pyramid/src) |
| M5Echo-Pyramid examples | [vendor/m5echo-pyramid/examples](vendor/m5echo-pyramid/examples) |

## Source URLs

| Source | URL |
| --- | --- |
| Voice Pyramid product page | https://docs.m5stack.com/en/atom/Echo_Pyramid |
| AtomS3R product page | https://docs.m5stack.com/en/core/AtomS3R |
| Voice Pyramid Arduino tutorial | https://docs.m5stack.com/en/arduino/projects/atomic/echo_pyramid |
| Voice Pyramid Home Assistant tutorial | https://docs.m5stack.com/en/homeassistant/voice_assistant/echo_pyramid |
| M5Echo-Pyramid library | https://github.com/m5stack/M5Echo-Pyramid |
| Official ESPHome Echo Pyramid YAML | https://github.com/m5stack/esphome-yaml/blob/main/examples/voice_assistant/echo_pyramid_example.yaml |
