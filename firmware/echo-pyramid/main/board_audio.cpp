#include "board.h"
#include "hermes_media.h"
#include "hermes_webrtc.h"
#include "main.h"

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_aec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_tdm.h"

namespace {

constexpr const char *TAG = "board_audio";
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kVoiceSampleRate = 16000;
constexpr size_t kVoiceDownsampleRatio = kSampleRate / kVoiceSampleRate;
static_assert(kSampleRate % kVoiceSampleRate == 0,
              "Audio sample rate must be an integer multiple of voice sample rate");
constexpr float kDefaultMicGainDb = 30.0f;
constexpr uint8_t kEs7210Mic1VoiceGainReg = 0x18;
constexpr uint8_t kEs7210Mic3ReferenceGainReg = 0x1E;
constexpr size_t kMicCaptureChannels = 4;
constexpr size_t kPlaybackChannels = 4;
constexpr size_t kAecMicSlot = 0;
constexpr size_t kAecRefSlot = 1;
constexpr uint16_t kMicReferenceSlotMask =
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(kAecMicSlot) |
    ESP_CODEC_DEV_MAKE_CHANNEL_MASK(kAecRefSlot);
constexpr bool kAecEnabled = true;
constexpr int kAecFilterLength = 4;
constexpr int kAecMicChannels = 1;
constexpr aec_mode_t kAecMode = AEC_MODE_VOIP_HIGH_PERF;
constexpr uint8_t kDefaultSpeakerVolumePercent = 50;
constexpr uint8_t kMaxSpeakerVolumePercent = 100;
constexpr uint8_t kSpeakerVolumeStepPercent = 5;
constexpr TickType_t kSwipeTimeoutTicks = pdMS_TO_TICKS(500);
constexpr TickType_t kLightsTickInterval = pdMS_TO_TICKS(50);
constexpr uint32_t kProbeSettlingFrames = 8;
constexpr uint8_t kDefaultLightBrightness = 80;
constexpr uint8_t kRgbStripCount = 2;
constexpr uint8_t kRgbGroupCount = 4;
constexpr uint8_t kRgbLedsPerGroup = 7;
constexpr uint8_t kRgbLedCount = kRgbLedsPerGroup * 2;
constexpr uint8_t kAw87559RegSysctrl = 0x01;
constexpr uint8_t kAw87559RegPaGain = 0x05;
constexpr uint8_t kAw87559PaGain24Db = 0x10;
constexpr size_t kSelfTestFrameSamples = kSampleRate / 50;
constexpr float kSelfTestAmplitude = 2500.0f;
constexpr bool kSelfTestTonesEnabled = false;
constexpr bool kInteractionTonesEnabled = true;

bool s_audio_initialized = false;
bool s_output_enabled = false;
i2c_master_bus_handle_t s_i2c_bus = nullptr;
i2c_master_dev_handle_t s_io_expander_dev = nullptr;
i2c_master_dev_handle_t s_touch_dev = nullptr;
i2c_master_dev_handle_t s_amp_dev = nullptr;
i2s_chan_handle_t s_tx_handle = nullptr;
i2s_chan_handle_t s_rx_handle = nullptr;
esp_codec_dev_handle_t s_mic_dev = nullptr;
esp_codec_dev_handle_t s_speaker_dev = nullptr;
int16_t *s_mic_stereo_buffer = nullptr;
size_t s_mic_stereo_buffer_bytes = 0;
aec_handle_t *s_aec_handle = nullptr;
int s_aec_frame_samples = 0;
int16_t *s_aec_mic_buffer = nullptr;
int16_t *s_aec_ref_buffer = nullptr;
int16_t *s_aec_output_buffer = nullptr;
int16_t *s_aec_fifo_buffer = nullptr;
size_t s_aec_fifo_offset = 0;
size_t s_aec_fifo_samples = 0;
int16_t *s_speaker_stereo_buffer = nullptr;
size_t s_speaker_stereo_buffer_bytes = 0;
uint32_t s_mic_read_count = 0;
int32_t s_last_mic_peak = 0;
int32_t s_last_ref_peak = 0;
int32_t s_last_aec_peak = 0;
int32_t s_last_slot_peaks[kMicCaptureChannels] = {0};
uint8_t s_speaker_volume_percent = kDefaultSpeakerVolumePercent;
uint8_t s_last_touch_state = 0;
uint8_t s_swipe_first_touch = 0;
TickType_t s_swipe_deadline = 0;
StatusState s_light_state = STATUS_BOOT;
TickType_t s_last_lights_tick = 0;
bool s_dance_mode = false;
bool s_custom_lights_enabled = false;
uint8_t s_custom_red = 0;
uint8_t s_custom_green = 0;
uint8_t s_custom_blue = 0;
char s_custom_pattern[16] = "solid";
bool s_talk_enabled = true;
int s_last_user_button_level = 1;
TickType_t s_last_user_button_toggle = 0;

uint8_t s_rgb_frame_1[kRgbLedCount * 4] = {0};
uint8_t s_rgb_frame_2[kRgbLedCount * 4] = {0};

typedef struct {
  uint32_t duration_ms;
  uint32_t frequency_hz;
  float amplitude;
  volatile bool done;
  esp_err_t result;
} AecProbePlaybackContext;

void dump_codec_registers(const char *name, esp_codec_dev_handle_t dev,
                          const uint8_t *registers, size_t register_count) {
  if (dev == nullptr) {
    ESP_LOGW(TAG, "%s codec unavailable for register dump", name);
    return;
  }

  for (size_t i = 0; i < register_count; ++i) {
    int value = 0;
    if (esp_codec_dev_read_reg(dev, registers[i], &value) == ESP_CODEC_DEV_OK) {
      ESP_LOGI(TAG, "%s reg[0x%02x] = 0x%02x", name, registers[i], value & 0xff);
    } else {
      ESP_LOGW(TAG, "%s reg[0x%02x] read failed", name, registers[i]);
    }
  }
}

esp_err_t play_tone(float frequency_hz, size_t duration_ms) {
  int16_t frame[kSelfTestFrameSamples] = {0};
  const size_t frames =
      duration_ms / (1000 / (kSampleRate / kSelfTestFrameSamples));
  const float phase_increment =
      2.0f * static_cast<float>(M_PI) * frequency_hz / kSampleRate;
  float phase = 0.0f;

  for (size_t frame_index = 0; frame_index < frames; ++frame_index) {
    for (size_t i = 0; i < kSelfTestFrameSamples; ++i) {
      frame[i] = static_cast<int16_t>(sinf(phase) * kSelfTestAmplitude);
      phase += phase_increment;
      if (phase >= 2.0f * static_cast<float>(M_PI)) {
        phase -= 2.0f * static_cast<float>(M_PI);
      }
    }
    ESP_RETURN_ON_ERROR(board_audio_write(frame, sizeof(frame)), TAG,
                        "Tone write failed");
  }
  return ESP_OK;
}

esp_err_t play_tone_enveloped(float frequency_hz, size_t duration_ms,
                              float amplitude, size_t attack_ms,
                              size_t release_ms) {
  int16_t frame[kSelfTestFrameSamples] = {0};
  const size_t frame_ms = 1000 / (kSampleRate / kSelfTestFrameSamples);
  const size_t frames = std::max<size_t>(1, duration_ms / frame_ms);
  const size_t total_samples = frames * kSelfTestFrameSamples;
  const size_t attack_samples = std::max<size_t>(
      1, (attack_ms * kSampleRate) / 1000);
  const size_t release_samples = std::max<size_t>(
      1, (release_ms * kSampleRate) / 1000);
  const float phase_increment =
      2.0f * static_cast<float>(M_PI) * frequency_hz / kSampleRate;
  float phase = 0.0f;
  size_t sample_index = 0;

  for (size_t frame_index = 0; frame_index < frames; ++frame_index) {
    for (size_t i = 0; i < kSelfTestFrameSamples; ++i) {
      float envelope = 1.0f;
      if (sample_index < attack_samples) {
        envelope = static_cast<float>(sample_index) /
                   static_cast<float>(attack_samples);
      } else if (sample_index + release_samples > total_samples) {
        envelope =
            static_cast<float>(total_samples - sample_index) /
            static_cast<float>(release_samples);
      }
      envelope = std::max(0.0f, std::min(1.0f, envelope));
      frame[i] = static_cast<int16_t>(sinf(phase) * amplitude * envelope);
      phase += phase_increment;
      if (phase >= 2.0f * static_cast<float>(M_PI)) {
        phase -= 2.0f * static_cast<float>(M_PI);
      }
      ++sample_index;
    }
    ESP_RETURN_ON_ERROR(board_audio_write(frame, sizeof(frame)), TAG,
                        "Enveloped tone write failed");
  }
  return ESP_OK;
}

esp_err_t play_silence(size_t duration_ms) {
  int16_t frame[kSelfTestFrameSamples] = {0};
  const size_t frames =
      duration_ms / (1000 / (kSampleRate / kSelfTestFrameSamples));
  for (size_t i = 0; i < frames; ++i) {
    ESP_RETURN_ON_ERROR(board_audio_write(frame, sizeof(frame)), TAG,
                        "Silence write failed");
  }
  return ESP_OK;
}

void aec_probe_playback_task(void *arg) {
  auto *ctx = static_cast<AecProbePlaybackContext *>(arg);
  int16_t frame[kSelfTestFrameSamples] = {0};
  const size_t frame_ms = 1000 / (kSampleRate / kSelfTestFrameSamples);
  const size_t frames = std::max<size_t>(1, ctx->duration_ms / frame_ms);
  const float phase_increment =
      2.0f * static_cast<float>(M_PI) *
      static_cast<float>(ctx->frequency_hz) / kSampleRate;
  float phase = 0.0f;
  uint32_t noise_state = 0x13579bdf;
  const bool use_noise = ctx->frequency_hz == 0;

  for (size_t frame_index = 0; frame_index < frames; ++frame_index) {
    for (size_t i = 0; i < kSelfTestFrameSamples; ++i) {
      if (use_noise) {
        noise_state = (noise_state * 1664525U) + 1013904223U;
        const int32_t centered =
            static_cast<int32_t>((noise_state >> 16) & 0xffffU) - 32768;
        frame[i] =
            static_cast<int16_t>(centered * ctx->amplitude / 32768.0f);
      } else {
        frame[i] = static_cast<int16_t>(sinf(phase) * ctx->amplitude);
        phase += phase_increment;
        if (phase >= 2.0f * static_cast<float>(M_PI)) {
          phase -= 2.0f * static_cast<float>(M_PI);
        }
      }
    }
    const esp_err_t err = board_audio_write(frame, sizeof(frame));
    if (err != ESP_OK) {
      ctx->result = err;
      break;
    }
  }

  ctx->done = true;
  vTaskDelete(nullptr);
}

esp_err_t add_i2c_device(uint16_t address, i2c_master_dev_handle_t *out_handle) {
  i2c_device_config_t cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = address,
      .scl_speed_hz = 400000,
      .scl_wait_us = 0,
      .flags =
          {
              .disable_ack_check = 0,
          },
  };
  return i2c_master_bus_add_device(s_i2c_bus, &cfg, out_handle);
}

esp_err_t write_i2c_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
  uint8_t payload[2] = {reg, value};
  return i2c_master_transmit(dev, payload, sizeof(payload), 100);
}

esp_err_t read_i2c_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *value) {
  return i2c_master_transmit_receive(dev, &reg, sizeof(reg), value, 1, 100);
}

esp_err_t update_i2c_bits(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask,
                          uint8_t value) {
  uint8_t current = 0;
  ESP_RETURN_ON_ERROR(read_i2c_reg(dev, reg, &current), TAG,
                      "Failed to read register 0x%02x", reg);
  current = (current & static_cast<uint8_t>(~mask)) | (value & mask);
  return write_i2c_reg(dev, reg, current);
}

esp_err_t write_i2c_regs(i2c_master_dev_handle_t dev, uint8_t reg,
                         const uint8_t *data, size_t data_len) {
  uint8_t buffer[16] = {0};
  if (data_len + 1 > sizeof(buffer)) {
    return ESP_ERR_INVALID_SIZE;
  }
  buffer[0] = reg;
  memcpy(buffer + 1, data, data_len);
  return i2c_master_transmit(dev, buffer, data_len + 1, 100);
}

void log_i2c_scan() {
  if (s_i2c_bus == nullptr) {
    return;
  }

  bool found_device = false;
  for (uint8_t address = 0x08; address < 0x78; ++address) {
    if (i2c_master_probe(s_i2c_bus, address, 20) == ESP_OK) {
      ESP_LOGI(TAG, "I2C device present at 0x%02x", address);
      found_device = true;
    }
  }

  if (!found_device) {
    ESP_LOGW(TAG,
             "No I2C devices detected on bus SDA=%d SCL=%d. If this is only an "
             "AtomS3R without the Echo Pyramid base, this is expected.",
             static_cast<int>(kEchoPyramidBoardConfig.i2c_sda),
             static_cast<int>(kEchoPyramidBoardConfig.i2c_scl));
  }
}

esp_err_t init_clock_generator() {
  i2c_master_dev_handle_t dev = nullptr;
  ESP_RETURN_ON_ERROR(add_i2c_device(kEchoPyramidBoardConfig.si5351_addr, &dev),
                      TAG, "Failed to add Si5351 device");

  constexpr uint64_t kXtalHz = 27000000;
  constexpr uint64_t kPllHz = 884736000;
  constexpr uint64_t kMultisynthDiv = 72;
  constexpr uint64_t kMclkHz = kPllHz / kMultisynthDiv;
  constexpr uint64_t kPllA = kPllHz / kXtalHz;
  constexpr uint64_t kPllRest = kPllHz % kXtalHz;
  constexpr uint64_t kPllC = 1000000;
  constexpr uint64_t kPllB = (kPllRest * kPllC) / kXtalHz;
  constexpr uint64_t kPllP1 =
      128 * kPllA + (128 * kPllB) / kPllC - 512;
  constexpr uint64_t kPllP2 =
      128 * kPllB - kPllC * ((128 * kPllB) / kPllC);
  constexpr uint64_t kPllP3 = kPllC;
  constexpr uint64_t kMsP1 = 128 * kMultisynthDiv - 512;
  constexpr uint8_t kPllAConfig[8] = {
      static_cast<uint8_t>((kPllP3 >> 8) & 0xFF),
      static_cast<uint8_t>(kPllP3 & 0xFF),
      static_cast<uint8_t>((kPllP1 >> 16) & 0x03),
      static_cast<uint8_t>((kPllP1 >> 8) & 0xFF),
      static_cast<uint8_t>(kPllP1 & 0xFF),
      static_cast<uint8_t>(((kPllP3 >> 12) & 0xF0) |
                           ((kPllP2 >> 16) & 0x0F)),
      static_cast<uint8_t>((kPllP2 >> 8) & 0xFF),
      static_cast<uint8_t>(kPllP2 & 0xFF),
  };
  constexpr uint8_t kMultiSynth1[8] = {
      0x00,
      0x01,
      static_cast<uint8_t>((kMsP1 >> 16) & 0x03),
      static_cast<uint8_t>((kMsP1 >> 8) & 0xFF),
      static_cast<uint8_t>(kMsP1 & 0xFF),
      0x00,
      0x00,
      0x00,
  };

  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 3, 0xFF), TAG,
                      "Failed to disable Si5351 outputs");
  vTaskDelay(pdMS_TO_TICKS(10));
  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 16, 0x80), TAG,
                      "Failed to power down Si5351 CLK0");
  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 17, 0x80), TAG,
                      "Failed to power down Si5351 CLK1");
  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 18, 0x80), TAG,
                      "Failed to power down Si5351 CLK2");
  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 183, 0xC0), TAG,
                      "Failed to configure Si5351 crystal load");
  ESP_RETURN_ON_ERROR(write_i2c_regs(dev, 26, kPllAConfig, sizeof(kPllAConfig)),
                      TAG, "Failed to configure Si5351 PLL");
  ESP_RETURN_ON_ERROR(write_i2c_regs(dev, 50, kMultiSynth1, sizeof(kMultiSynth1)),
                      TAG, "Failed to configure Si5351 multisynth");
  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 17, 0x4F), TAG,
                      "Failed to configure Si5351 CLK1");
  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 16, 0x80), TAG,
                      "Failed to keep Si5351 CLK0 powered down");
  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 18, 0x80), TAG,
                      "Failed to keep Si5351 CLK2 powered down");
  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 177, 0xA0), TAG,
                      "Failed to reset Si5351 PLL");
  vTaskDelay(pdMS_TO_TICKS(10));
  ESP_RETURN_ON_ERROR(write_i2c_reg(dev, 3, 0xFD), TAG,
                      "Failed to enable Si5351 CLK1");

  i2c_master_bus_rm_device(dev);
  ESP_LOGI(TAG, "Si5351 CLK1 configured for %lu Hz MCLK",
           static_cast<unsigned long>(kMclkHz));
  return ESP_OK;
}

esp_err_t set_speaker_enable(bool enabled) {
  if (s_io_expander_dev == nullptr) {
    return ESP_OK;
  }

  const uint8_t speaker_mask =
      static_cast<uint8_t>(1u << kEchoPyramidBoardConfig.speaker_enable_pin);
  return update_i2c_bits(s_io_expander_dev, 0x05,
                         speaker_mask, enabled ? speaker_mask : 0);
}

esp_err_t init_io_expander() {
  esp_err_t err =
      add_i2c_device(kEchoPyramidBoardConfig.io_expander_addr, &s_io_expander_dev);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "PI4IOE5V6408 not present at 0x%02x: %s",
             kEchoPyramidBoardConfig.io_expander_addr, esp_err_to_name(err));
    s_io_expander_dev = nullptr;
    return ESP_OK;
  }

  uint8_t chip_id = 0;
  err = read_i2c_reg(s_io_expander_dev, 0x01, &chip_id);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "PI4IOE5V6408 probe failed at 0x%02x: %s",
             kEchoPyramidBoardConfig.io_expander_addr, esp_err_to_name(err));
    i2c_master_bus_rm_device(s_io_expander_dev);
    s_io_expander_dev = nullptr;
    return ESP_OK;
  }
  ESP_LOGI(TAG, "PI4IOE5V6408 detected at 0x%02x (id=0x%02x)",
           kEchoPyramidBoardConfig.io_expander_addr, chip_id);

  const uint8_t speaker_mask =
      static_cast<uint8_t>(1u << kEchoPyramidBoardConfig.speaker_enable_pin);

  ESP_RETURN_ON_ERROR(update_i2c_bits(s_io_expander_dev, 0x07, speaker_mask, 0),
                      TAG, "Failed to disable speaker high-Z");
  ESP_RETURN_ON_ERROR(update_i2c_bits(s_io_expander_dev, 0x03, speaker_mask,
                                      speaker_mask),
                      TAG, "Failed to configure speaker enable as output");
  ESP_RETURN_ON_ERROR(set_speaker_enable(s_output_enabled), TAG,
                      "Failed to set initial speaker enable state");
  return ESP_OK;
}

esp_err_t init_amplifier() {
  ESP_RETURN_ON_ERROR(add_i2c_device(kEchoPyramidBoardConfig.aw87559_addr,
                                     &s_amp_dev),
                      TAG, "Failed to add AW87559 device");
  ESP_RETURN_ON_ERROR(write_i2c_reg(s_amp_dev, 0x00, 0xFF), TAG,
                      "Failed to probe AW87559");
  ESP_RETURN_ON_ERROR(write_i2c_reg(s_amp_dev, kAw87559RegSysctrl, 0x00), TAG,
                      "Failed to keep AW87559 muted during bring-up");
  ESP_RETURN_ON_ERROR(write_i2c_reg(s_amp_dev, kAw87559RegPaGain,
                                    kAw87559PaGain24Db),
                      TAG, "Failed to configure AW87559 PA gain");
  return ESP_OK;
}

esp_err_t read_touch_state(uint8_t *touch_state);

esp_err_t init_touch_controller() {
  esp_err_t err =
      add_i2c_device(kEchoPyramidBoardConfig.touch_addr, &s_touch_dev);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Touch controller not present at 0x%02x: %s",
             kEchoPyramidBoardConfig.touch_addr, esp_err_to_name(err));
    s_touch_dev = nullptr;
    return ESP_OK;
  }

  uint8_t initial_touch_state = 0;
  err = read_touch_state(&initial_touch_state);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Touch controller probe failed at 0x%02x: %s",
             kEchoPyramidBoardConfig.touch_addr, esp_err_to_name(err));
    i2c_master_bus_rm_device(s_touch_dev);
    s_touch_dev = nullptr;
    return ESP_OK;
  }
  s_last_touch_state = initial_touch_state;
  ESP_LOGI(TAG, "STM32 touch/RGB controller ready at 0x%02x touch=0x%02x",
           kEchoPyramidBoardConfig.touch_addr, initial_touch_state);
  return ESP_OK;
}

esp_err_t read_touch_state(uint8_t *touch_state) {
  if (touch_state == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_touch_dev == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t reg = 0x00;
  uint8_t raw[4] = {0};
  ESP_RETURN_ON_ERROR(
      i2c_master_transmit_receive(s_touch_dev, &reg, sizeof(reg), raw,
                                  sizeof(raw), 100),
      TAG, "Failed to read touch state");

  uint8_t mask = 0;
  if (raw[0]) {
    mask |= 0x01;
  }
  if (raw[1]) {
    mask |= 0x02;
  }
  if (raw[2]) {
    mask |= 0x04;
  }
  if (raw[3]) {
    mask |= 0x08;
  }
  *touch_state = mask;
  return ESP_OK;
}

uint8_t rgb_group_base_reg(uint8_t group) {
  switch (group) {
    case 0:
      return 0x20;
    case 1:
      return 0x3C;
    case 2:
      return 0x7C;
    case 3:
      return 0x60;
    default:
      return 0x20;
  }
}

esp_err_t set_light_brightness(uint8_t channel, uint8_t brightness) {
  if (s_touch_dev == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (channel < 1 || channel > kRgbStripCount) {
    return ESP_ERR_INVALID_ARG;
  }
  if (brightness > 100) {
    brightness = 100;
  }
  const uint8_t scaled =
      static_cast<uint8_t>((static_cast<uint16_t>(brightness) * 255) / 100);
  const uint8_t reg = static_cast<uint8_t>(0x10 + (channel - 1));
  return write_i2c_reg(s_touch_dev, reg, scaled);
}

void clear_rgb_frame(uint8_t *frame) { memset(frame, 0, kRgbLedCount * 4); }

void set_rgb_led(uint8_t *frame, size_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= kRgbLedCount) {
    return;
  }
  const size_t offset = index * 4;
  frame[offset + 0] = b;
  frame[offset + 1] = g;
  frame[offset + 2] = r;
  frame[offset + 3] = 0x00;
}

void fill_rgb_frame(uint8_t *frame, uint8_t r, uint8_t g, uint8_t b) {
  for (size_t i = 0; i < kRgbLedCount; ++i) {
    set_rgb_led(frame, i, r, g, b);
  }
}

uint8_t triangle_wave(uint32_t phase, uint32_t period, uint8_t minimum,
                      uint8_t maximum) {
  if (period == 0 || maximum <= minimum) {
    return minimum;
  }

  const uint32_t half_period = period / 2;
  uint32_t offset = phase % period;
  if (offset > half_period) {
    offset = period - offset;
  }

  return static_cast<uint8_t>(
      minimum + ((maximum - minimum) * offset) /
                    (half_period == 0 ? 1 : half_period));
}

void render_idle_lights(uint32_t tick_count) {
  constexpr uint32_t kRollPeriodTicks = 6400;
  constexpr float kGlowRadius = 4.0f;
  const uint32_t period_position = tick_count % kRollPeriodTicks;
  const float half_period = static_cast<float>(kRollPeriodTicks) / 2.0f;
  const float travel = static_cast<float>(kRgbLedCount - 1);
  const float head =
      period_position <= (kRollPeriodTicks / 2)
          ? (static_cast<float>(period_position) / half_period) * travel
          : ((static_cast<float>(kRollPeriodTicks - period_position) /
              half_period) *
             travel);

  for (size_t i = 0; i < kRgbLedCount; ++i) {
    const float distance = fabsf(static_cast<float>(i) - head);
    const float glow =
        distance < kGlowRadius ? (1.0f - (distance / kGlowRadius)) : 0.0f;
    const uint8_t warmth =
        static_cast<uint8_t>(18.0f + (glow * glow * 70.0f));
    const uint8_t r = warmth;
    const uint8_t g = static_cast<uint8_t>((warmth * 3) / 5);
    const uint8_t b = static_cast<uint8_t>(warmth / 7);
    set_rgb_led(s_rgb_frame_1, i, r, g, b);
    set_rgb_led(s_rgb_frame_2, kRgbLedCount - 1 - i, r, g, b);
  }
}

void render_media_lights(uint32_t tick_count) {
  const uint8_t breath = triangle_wave(tick_count, 72, 18, 90);
  const size_t head = (tick_count / 8) % kRgbLedCount;
  for (size_t i = 0; i < kRgbLedCount; ++i) {
    const size_t distance = (head + kRgbLedCount - i) % kRgbLedCount;
    const uint8_t lift =
        distance < 4 ? static_cast<uint8_t>(120 - distance * 24) : breath;
    const uint8_t r = static_cast<uint8_t>(lift / 8);
    const uint8_t g = lift;
    const uint8_t b = static_cast<uint8_t>(lift / 4);
    set_rgb_led(s_rgb_frame_1, i, r, g, b);
    set_rgb_led(s_rgb_frame_2, kRgbLedCount - 1 - i, r, g, b);
  }
}

void render_listening_lights(uint32_t tick_count) {
  clear_rgb_frame(s_rgb_frame_1);
  clear_rgb_frame(s_rgb_frame_2);

  constexpr uint8_t kBaseRed = 10;
  constexpr uint8_t kBaseGreen = 0;
  constexpr uint8_t kBaseBlue = 24;
  constexpr uint8_t kPeakRed = 56;
  constexpr uint8_t kPeakBlue = 112;
  constexpr size_t kGlowSpan = 6;
  constexpr uint32_t kRollStepTicks = 22;
  constexpr uint32_t kResetGap = 4;

  const int32_t head = static_cast<int32_t>(
                           (tick_count / kRollStepTicks) %
                           (kRgbLedCount + kGlowSpan + kResetGap)) -
                       static_cast<int32_t>(kGlowSpan);

  for (size_t i = 0; i < kRgbLedCount; ++i) {
    uint8_t r = kBaseRed;
    uint8_t g = kBaseGreen;
    uint8_t b = kBaseBlue;

    const int32_t distance = head - static_cast<int32_t>(i);
    if (distance >= 0 && distance < static_cast<int32_t>(kGlowSpan)) {
      const uint8_t mix =
          static_cast<uint8_t>(((kGlowSpan - distance) * 255) / kGlowSpan);
      r = static_cast<uint8_t>(kBaseRed + ((kPeakRed - kBaseRed) * mix) / 255);
      b =
          static_cast<uint8_t>(kBaseBlue + ((kPeakBlue - kBaseBlue) * mix) / 255);
    }

    set_rgb_led(s_rgb_frame_1, i, r, g, b);
    set_rgb_led(s_rgb_frame_2, kRgbLedCount - 1 - i, r, g, b);
  }
}

void render_speaking_lights(uint32_t tick_count) {
  clear_rgb_frame(s_rgb_frame_1);
  clear_rgb_frame(s_rgb_frame_2);
  const size_t head = (tick_count / 10) % kRgbLedCount;
  for (size_t i = 0; i < kRgbLedCount; ++i) {
    const size_t distance =
        (head + kRgbLedCount - i) % kRgbLedCount;
    const uint8_t scale = distance < 5 ? static_cast<uint8_t>(220 - (distance * 40))
                                       : 20;
    set_rgb_led(s_rgb_frame_1, i, scale, (scale * 3) / 8, 0);
    set_rgb_led(s_rgb_frame_2, kRgbLedCount - 1 - i, scale, (scale * 3) / 8, 0);
  }
}

void render_error_lights(uint32_t tick_count) {
  const uint8_t pulse = triangle_wave(tick_count, 36, 20, 200);
  fill_rgb_frame(s_rgb_frame_1, pulse, 0, 0);
  fill_rgb_frame(s_rgb_frame_2, pulse, 0, 0);
}

void render_thinking_lights(uint32_t tick_count) {
  clear_rgb_frame(s_rgb_frame_1);
  clear_rgb_frame(s_rgb_frame_2);
  const uint8_t breath = triangle_wave(tick_count, 56, 12, 120);
  const size_t head = (tick_count / 5) % kRgbLedCount;
  for (size_t i = 0; i < kRgbLedCount; ++i) {
    const size_t distance = (head + kRgbLedCount - i) % kRgbLedCount;
    const uint8_t spark =
        distance < 4 ? static_cast<uint8_t>(180 - distance * 38) : breath;
    const uint8_t r = static_cast<uint8_t>(spark / 5);
    const uint8_t g = static_cast<uint8_t>((spark * 3) / 8);
    const uint8_t b = static_cast<uint8_t>(spark);
    set_rgb_led(s_rgb_frame_1, i, r, g, b);
    set_rgb_led(s_rgb_frame_2, kRgbLedCount - 1 - i, r, g, b);
  }
}

void render_custom_lights(uint32_t tick_count) {
  uint8_t r = s_custom_red;
  uint8_t g = s_custom_green;
  uint8_t b = s_custom_blue;

  if (strcmp(s_custom_pattern, "pulse") == 0) {
    const uint8_t pulse = triangle_wave(tick_count, 42, 18, 220);
    r = static_cast<uint8_t>((static_cast<uint16_t>(s_custom_red) * pulse) / 220);
    g = static_cast<uint8_t>((static_cast<uint16_t>(s_custom_green) * pulse) / 220);
    b = static_cast<uint8_t>((static_cast<uint16_t>(s_custom_blue) * pulse) / 220);
    fill_rgb_frame(s_rgb_frame_1, r, g, b);
    fill_rgb_frame(s_rgb_frame_2, r, g, b);
    return;
  }

  fill_rgb_frame(s_rgb_frame_1, r, g, b);
  fill_rgb_frame(s_rgb_frame_2, r, g, b);
}

void render_dance_lights(uint32_t tick_count) {
  clear_rgb_frame(s_rgb_frame_1);
  clear_rgb_frame(s_rgb_frame_2);
  const size_t head = (tick_count / 4) % kRgbLedCount;
  for (size_t i = 0; i < kRgbLedCount; ++i) {
    const size_t distance = (head + kRgbLedCount - i) % kRgbLedCount;
    const uint8_t hot = distance < 5 ? static_cast<uint8_t>(240 - distance * 38) : 18;
    const uint8_t r = distance < 5 ? hot : static_cast<uint8_t>(8 + ((i * 13 + tick_count) % 28));
    const uint8_t g = distance < 5 ? static_cast<uint8_t>((hot * 7) / 10) : static_cast<uint8_t>(18 + ((i * 7) % 36));
    const uint8_t b = distance < 5 ? static_cast<uint8_t>(hot / 6) : static_cast<uint8_t>(4 + ((i * 3) % 20));
    set_rgb_led(s_rgb_frame_1, i, r, g, b);
    set_rgb_led(s_rgb_frame_2, kRgbLedCount - 1 - i, b, r, g);
  }
}

esp_err_t flush_rgb_group(uint8_t group, const uint8_t *strip_frame,
                          size_t group_offset) {
  if (s_touch_dev == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (group >= kRgbGroupCount || strip_frame == nullptr ||
      group_offset + kRgbLedsPerGroup > kRgbLedCount) {
    return ESP_ERR_INVALID_ARG;
  }

  const uint8_t base = rgb_group_base_reg(group);
  esp_err_t result = ESP_OK;
  for (size_t led = 0; led < kRgbLedsPerGroup; ++led) {
    const size_t hardware_index =
        (group == 0 || group == 1) ? (kRgbLedsPerGroup - 1 - led) : led;
    const uint8_t reg = static_cast<uint8_t>(base + (hardware_index * 4));
    const uint8_t *source = strip_frame + ((group_offset + led) * 4);
    const esp_err_t err = write_i2c_regs(s_touch_dev, reg, source, 4);
    if (err != ESP_OK) {
      result = err;
    }
  }
  return result;
}

esp_err_t flush_rgb_frames(void) {
  ESP_RETURN_ON_ERROR(flush_rgb_group(0, s_rgb_frame_1, 0), TAG,
                      "Failed to flush RGB group 0");
  ESP_RETURN_ON_ERROR(flush_rgb_group(1, s_rgb_frame_1, kRgbLedsPerGroup), TAG,
                      "Failed to flush RGB group 1");
  ESP_RETURN_ON_ERROR(flush_rgb_group(2, s_rgb_frame_2, 0), TAG,
                      "Failed to flush RGB group 2");
  ESP_RETURN_ON_ERROR(flush_rgb_group(3, s_rgb_frame_2, kRgbLedsPerGroup), TAG,
                      "Failed to flush RGB group 3");
  return ESP_OK;
}

void render_lights_for_state(uint32_t tick_count) {
  if (s_dance_mode) {
    render_dance_lights(tick_count);
    return;
  }
  if (s_custom_lights_enabled) {
    render_custom_lights(tick_count);
    return;
  }

  switch (s_light_state) {
    case STATUS_LISTENING:
      render_listening_lights(tick_count);
      break;
    case STATUS_BOT_SPEAKING:
      render_speaking_lights(tick_count);
      break;
    case STATUS_SIGNALING:
      render_thinking_lights(tick_count);
      break;
    case STATUS_ERROR:
    case STATUS_DISCONNECTED:
      render_error_lights(tick_count);
      break;
    case STATUS_MEDIA_PLAYING:
      render_media_lights(tick_count);
      break;
    case STATUS_BOOT:
    case STATUS_WIFI_CONNECTING:
      render_thinking_lights(tick_count);
      break;
    case STATUS_CONNECTED_IDLE:
    default:
      render_idle_lights(tick_count);
      break;
  }
}

esp_err_t apply_speaker_volume() {
  if (s_speaker_dev == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  return esp_codec_dev_set_out_vol(s_speaker_dev, s_speaker_volume_percent);
}

esp_err_t init_i2s() {
  i2s_chan_config_t chan_cfg = {};
  chan_cfg.id = kEchoPyramidBoardConfig.i2s_port;
  chan_cfg.role = I2S_ROLE_MASTER;
  chan_cfg.dma_desc_num = 6;
  chan_cfg.dma_frame_num = 240;
  chan_cfg.auto_clear_after_cb = true;
  chan_cfg.auto_clear_before_cb = false;
  chan_cfg.allow_pd = false;
  chan_cfg.intr_priority = 0;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle),
                      TAG, "Failed to allocate I2S channels");

  i2s_std_config_t tx_cfg = {};
  tx_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  tx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  tx_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO);
  tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  tx_cfg.slot_cfg.ws_width = 32;
  tx_cfg.gpio_cfg.mclk = GPIO_NUM_NC;
  tx_cfg.gpio_cfg.bclk = kEchoPyramidBoardConfig.i2s_bclk;
  tx_cfg.gpio_cfg.ws = kEchoPyramidBoardConfig.i2s_lrck;
  tx_cfg.gpio_cfg.dout = kEchoPyramidBoardConfig.i2s_dout;
  tx_cfg.gpio_cfg.din = GPIO_NUM_NC;
  tx_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  tx_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  tx_cfg.gpio_cfg.invert_flags.ws_inv = false;

  i2s_std_config_t rx_cfg = tx_cfg;
  rx_cfg.gpio_cfg.dout = GPIO_NUM_NC;
  rx_cfg.gpio_cfg.din = kEchoPyramidBoardConfig.i2s_din;

  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &tx_cfg), TAG,
                      "Failed to init I2S TX standard mode");
  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &rx_cfg), TAG,
                      "Failed to init I2S RX standard mode");
  return ESP_OK;
}

esp_err_t configure_stream_i2s_mode() {
  if (s_tx_handle == nullptr || s_rx_handle == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(s_tx_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_channel_disable(s_rx_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_del_channel(s_tx_handle));
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2s_del_channel(s_rx_handle));
  s_tx_handle = nullptr;
  s_rx_handle = nullptr;

  i2s_chan_config_t chan_cfg = {};
  chan_cfg.id = kEchoPyramidBoardConfig.i2s_port;
  chan_cfg.role = I2S_ROLE_MASTER;
  chan_cfg.dma_desc_num = 6;
  chan_cfg.dma_frame_num = 240;
  chan_cfg.auto_clear_after_cb = true;
  chan_cfg.auto_clear_before_cb = false;
  chan_cfg.allow_pd = false;
  chan_cfg.intr_priority = 0;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle),
                      TAG, "Failed to allocate TDM I2S channels");

  i2s_tdm_config_t tdm_cfg = {};
  tdm_cfg.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(kSampleRate);
  tdm_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  tdm_cfg.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
      static_cast<i2s_tdm_slot_mask_t>(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                       I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
  tdm_cfg.slot_cfg.total_slot = 4;
  tdm_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
  tdm_cfg.gpio_cfg.mclk = GPIO_NUM_NC;
  tdm_cfg.gpio_cfg.bclk = kEchoPyramidBoardConfig.i2s_bclk;
  tdm_cfg.gpio_cfg.ws = kEchoPyramidBoardConfig.i2s_lrck;
  tdm_cfg.gpio_cfg.dout = kEchoPyramidBoardConfig.i2s_dout;
  tdm_cfg.gpio_cfg.din = kEchoPyramidBoardConfig.i2s_din;
  tdm_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  tdm_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  tdm_cfg.gpio_cfg.invert_flags.ws_inv = false;

  ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_tx_handle, &tdm_cfg), TAG,
                      "Failed to init I2S TX TDM mode");
  ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx_handle, &tdm_cfg), TAG,
                      "Failed to init I2S RX TDM mode");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG,
                      "Failed to enable I2S RX");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG,
                      "Failed to enable I2S TX");

  ESP_LOGI(TAG, "I2S stream mode ready: TX/RX 4-slot TDM 16-bit slots");
  return ESP_OK;
}

esp_err_t init_codecs() {
  audio_codec_i2s_cfg_t i2s_cfg = {};
  i2s_cfg.port = static_cast<uint8_t>(kEchoPyramidBoardConfig.i2s_port);
  i2s_cfg.rx_handle = s_rx_handle;
  i2s_cfg.tx_handle = s_tx_handle;

  audio_codec_i2c_cfg_t es8311_i2c_cfg = {};
  es8311_i2c_cfg.port = static_cast<uint8_t>(kEchoPyramidBoardConfig.i2c_port);
  es8311_i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
  es8311_i2c_cfg.bus_handle = s_i2c_bus;

  es8311_codec_cfg_t es8311_cfg = {};
  es8311_cfg.ctrl_if = audio_codec_new_i2c_ctrl(&es8311_i2c_cfg);
  es8311_cfg.gpio_if = audio_codec_new_gpio();
  es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
  es8311_cfg.pa_pin = GPIO_NUM_NC;
  es8311_cfg.pa_reverted = false;
  es8311_cfg.master_mode = false;
  es8311_cfg.use_mclk = true;
  es8311_cfg.digital_mic = false;
  es8311_cfg.invert_mclk = false;
  es8311_cfg.invert_sclk = false;
  es8311_cfg.hw_gain.pa_voltage = 5.0;
  es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
  es8311_cfg.hw_gain.pa_gain = 0.0;
  es8311_cfg.no_dac_ref = false;
  es8311_cfg.mclk_div = 256;

  esp_codec_dev_cfg_t speaker_cfg = {};
  speaker_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  speaker_cfg.codec_if = es8311_codec_new(&es8311_cfg);
  speaker_cfg.data_if = audio_codec_new_i2s_data(&i2s_cfg);
  s_speaker_dev = esp_codec_dev_new(&speaker_cfg);
  if (s_speaker_dev == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  audio_codec_i2c_cfg_t es7210_i2c_cfg = {};
  es7210_i2c_cfg.port = static_cast<uint8_t>(kEchoPyramidBoardConfig.i2c_port);
  es7210_i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
  es7210_i2c_cfg.bus_handle = s_i2c_bus;

  es7210_codec_cfg_t es7210_cfg = {};
  es7210_cfg.ctrl_if = audio_codec_new_i2c_ctrl(&es7210_i2c_cfg);
  es7210_cfg.master_mode = false;
  es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC3;
  es7210_cfg.mclk_src = ES7210_MCLK_FROM_PAD;
  es7210_cfg.mclk_div = 256;

  esp_codec_dev_cfg_t mic_cfg = {};
  mic_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
  mic_cfg.codec_if = es7210_codec_new(&es7210_cfg);
  mic_cfg.data_if = audio_codec_new_i2s_data(&i2s_cfg);
  s_mic_dev = esp_codec_dev_new(&mic_cfg);
  if (s_mic_dev == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  esp_codec_dev_sample_info_t sample_info = {};
  sample_info.bits_per_sample = 16;
  sample_info.channel = kPlaybackChannels;
  sample_info.channel_mask = 0;
  sample_info.sample_rate = kSampleRate;
  sample_info.mclk_multiple = 0;
  ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_speaker_dev, &sample_info), TAG,
                      "Failed to open speaker codec");
  sample_info.channel = kMicCaptureChannels;
  sample_info.channel_mask = kMicReferenceSlotMask;
  ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_mic_dev, &sample_info), TAG,
                      "Failed to open microphone codec");
  ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_mic_dev, kDefaultMicGainDb), TAG,
                      "Failed to set microphone gain");
  ESP_RETURN_ON_ERROR(
      esp_codec_dev_set_in_channel_gain(s_mic_dev,
                                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2),
                                        30.0f),
      TAG, "Failed to set ES7210 MIC3 reference gain");

  uint8_t clock_reg = 0;
  int clock_value = 0;
  ESP_RETURN_ON_ERROR(esp_codec_dev_read_reg(s_mic_dev, 0x01, &clock_value),
                      TAG, "Failed to read ES7210 clock register");
  clock_reg = static_cast<uint8_t>(clock_value & 0xff);
  clock_reg &= static_cast<uint8_t>(~0x3F);
  ESP_RETURN_ON_ERROR(esp_codec_dev_write_reg(s_mic_dev, 0x01, clock_reg), TAG,
                      "Failed to enable ES7210 ADC clocks");
  ESP_RETURN_ON_ERROR(esp_codec_dev_write_reg(s_mic_dev, 0x08, 0x20), TAG,
                      "Failed to set ES7210 M5 slave mode register");
  ESP_RETURN_ON_ERROR(esp_codec_dev_write_reg(s_mic_dev, 0x13, 0x00), TAG,
                      "Failed to disable ES7210 automute");
  ESP_RETURN_ON_ERROR(esp_codec_dev_write_reg(s_mic_dev, 0x12, 0x02), TAG,
                      "Failed to set ES7210 4-slot TDM mode");
  ESP_RETURN_ON_ERROR(esp_codec_dev_write_reg(s_mic_dev, 0x43,
                                              kEs7210Mic1VoiceGainReg),
                      TAG,
                      "Failed to set ES7210 MIC1 voice gain register");
  ESP_RETURN_ON_ERROR(esp_codec_dev_write_reg(s_mic_dev, 0x45,
                                              kEs7210Mic3ReferenceGainReg),
                      TAG,
                      "Failed to set ES7210 MIC3 reference gain register");
  int reg11 = 0;
  int reg12 = 0;
  int mic1_gain = 0;
  int mic3_gain = 0;
  ESP_RETURN_ON_ERROR(esp_codec_dev_read_reg(s_mic_dev, 0x11, &reg11), TAG,
                      "Failed to read ES7210 format register");
  ESP_RETURN_ON_ERROR(esp_codec_dev_read_reg(s_mic_dev, 0x12, &reg12), TAG,
                      "Failed to read ES7210 interface register");
  ESP_RETURN_ON_ERROR(esp_codec_dev_read_reg(s_mic_dev, 0x43, &mic1_gain), TAG,
                      "Failed to read ES7210 MIC1 gain register");
  ESP_RETURN_ON_ERROR(esp_codec_dev_read_reg(s_mic_dev, 0x45, &mic3_gain), TAG,
                      "Failed to read ES7210 MIC3 gain register");
  ESP_LOGI(TAG,
           "ES7210 MIC1/MIC3 reference ready clock=0x%02x fmt=0x%02x if2=0x%02x "
           "mic1=0x%02x mic3=0x%02x",
           clock_reg, reg11 & 0xff, reg12 & 0xff, mic1_gain & 0xff,
           mic3_gain & 0xff);
  ESP_RETURN_ON_ERROR(configure_stream_i2s_mode(), TAG,
                      "Failed to restore final I2S streaming mode");
  ESP_RETURN_ON_ERROR(apply_speaker_volume(), TAG,
                      "Failed to set speaker volume");
  return ESP_OK;
}

esp_err_t ensure_stereo_read_buffer(size_t mono_sample_count) {
  const size_t hardware_sample_count = mono_sample_count * kVoiceDownsampleRatio;
  const size_t stereo_bytes =
      hardware_sample_count * kMicCaptureChannels * sizeof(int16_t);
  if (s_mic_stereo_buffer_bytes >= stereo_bytes) {
    return ESP_OK;
  }

  auto *new_buffer = static_cast<int16_t *>(
      realloc(s_mic_stereo_buffer, stereo_bytes));
  if (new_buffer == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  s_mic_stereo_buffer = new_buffer;
  s_mic_stereo_buffer_bytes = stereo_bytes;
  return ESP_OK;
}

void update_capture_peaks(size_t mono_sample_count) {
  const size_t hardware_sample_count = mono_sample_count * kVoiceDownsampleRatio;
  int32_t slot_peaks[kMicCaptureChannels] = {0};
  for (size_t i = 0; i < hardware_sample_count; ++i) {
    for (size_t channel = 0; channel < kMicCaptureChannels; ++channel) {
      const int32_t sample =
          s_mic_stereo_buffer[(i * kMicCaptureChannels) + channel];
      const int32_t magnitude = sample < 0 ? -sample : sample;
      if (magnitude > slot_peaks[channel]) {
        slot_peaks[channel] = magnitude;
      }
    }
  }

  for (size_t channel = 0; channel < kMicCaptureChannels; ++channel) {
    s_last_slot_peaks[channel] = slot_peaks[channel];
  }
  s_last_mic_peak = slot_peaks[kAecMicSlot];
  s_last_ref_peak = slot_peaks[kAecRefSlot];
}

esp_err_t read_stereo_capture(size_t mono_sample_count) {
  ESP_RETURN_ON_ERROR(ensure_stereo_read_buffer(mono_sample_count), TAG,
                      "Failed to allocate stereo mic buffer");
  const size_t hardware_sample_count = mono_sample_count * kVoiceDownsampleRatio;
  const size_t stereo_bytes =
      hardware_sample_count * kMicCaptureChannels * sizeof(int16_t);
  size_t bytes_read = 0;
  esp_err_t err = i2s_channel_read(s_rx_handle, s_mic_stereo_buffer,
                                   stereo_bytes, &bytes_read, portMAX_DELAY);
  if (err != ESP_OK) {
    return err;
  }
  if (bytes_read != stereo_bytes) {
    ESP_LOGW(TAG, "Short I2S capture read: %u/%u bytes",
             static_cast<unsigned>(bytes_read),
             static_cast<unsigned>(stereo_bytes));
    return ESP_ERR_TIMEOUT;
  }
  update_capture_peaks(mono_sample_count);
  return ESP_OK;
}

void copy_raw_mic_channel(int16_t *dest, size_t mono_sample_count) {
  for (size_t i = 0; i < mono_sample_count; ++i) {
    int32_t sum = 0;
    const size_t base = i * kVoiceDownsampleRatio * kMicCaptureChannels;
    for (size_t j = 0; j < kVoiceDownsampleRatio; ++j) {
      sum += s_mic_stereo_buffer[base + (j * kMicCaptureChannels)];
    }
    dest[i] = static_cast<int16_t>(sum / static_cast<int32_t>(kVoiceDownsampleRatio));
  }
  s_last_aec_peak = 0;
}

int16_t downsample_capture_slot(size_t voice_index, size_t channel) {
  int32_t sum = 0;
  const size_t base = voice_index * kVoiceDownsampleRatio * kMicCaptureChannels;
  for (size_t j = 0; j < kVoiceDownsampleRatio; ++j) {
    sum += s_mic_stereo_buffer[base + (j * kMicCaptureChannels) + channel];
  }
  return static_cast<int16_t>(sum / static_cast<int32_t>(kVoiceDownsampleRatio));
}

esp_err_t read_raw_mic(int16_t *dest, size_t mono_sample_count) {
  ESP_RETURN_ON_ERROR(read_stereo_capture(mono_sample_count), TAG,
                      "Failed to read stereo mic capture");
  copy_raw_mic_channel(dest, mono_sample_count);
  return ESP_OK;
}

int32_t peak_for_samples(const int16_t *samples, size_t sample_count) {
  int32_t peak = 0;
  for (size_t i = 0; i < sample_count; ++i) {
    int32_t sample = samples[i];
    if (sample < 0) {
      sample = -sample;
    }
    if (sample > peak) {
      peak = sample;
    }
  }
  return peak;
}

void *alloc_aligned_audio(size_t bytes) {
  return heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT);
}

esp_err_t init_aec() {
  if (!kAecEnabled) {
    return ESP_OK;
  }

  s_aec_handle =
      aec_create(kVoiceSampleRate, kAecFilterLength, kAecMicChannels, kAecMode);
  if (s_aec_handle == nullptr) {
    ESP_LOGW(TAG, "ESP-SR AEC unavailable; using raw mic channel");
    return ESP_OK;
  }

  s_aec_frame_samples = aec_get_chunksize(s_aec_handle);
  if (s_aec_frame_samples <= 0) {
    ESP_LOGW(TAG, "ESP-SR AEC returned invalid frame size; using raw mic channel");
    aec_destroy(s_aec_handle);
    s_aec_handle = nullptr;
    s_aec_frame_samples = 0;
    return ESP_OK;
  }

  const size_t frame_bytes =
      static_cast<size_t>(s_aec_frame_samples) * sizeof(int16_t);
  s_aec_mic_buffer = static_cast<int16_t *>(alloc_aligned_audio(frame_bytes));
  s_aec_ref_buffer = static_cast<int16_t *>(alloc_aligned_audio(frame_bytes));
  s_aec_output_buffer = static_cast<int16_t *>(alloc_aligned_audio(frame_bytes));
  s_aec_fifo_buffer = static_cast<int16_t *>(alloc_aligned_audio(frame_bytes));
  if (s_aec_mic_buffer == nullptr || s_aec_ref_buffer == nullptr ||
      s_aec_output_buffer == nullptr || s_aec_fifo_buffer == nullptr) {
    ESP_LOGW(TAG, "Failed to allocate ESP-SR AEC buffers; using raw mic channel");
    if (s_aec_handle != nullptr) {
      aec_destroy(s_aec_handle);
      s_aec_handle = nullptr;
    }
    free(s_aec_mic_buffer);
    free(s_aec_ref_buffer);
    free(s_aec_output_buffer);
    free(s_aec_fifo_buffer);
    s_aec_mic_buffer = nullptr;
    s_aec_ref_buffer = nullptr;
    s_aec_output_buffer = nullptr;
    s_aec_fifo_buffer = nullptr;
    s_aec_frame_samples = 0;
    return ESP_OK;
  }

  ESP_LOGI(TAG, "ESP-SR AEC enabled mode=%s frame_samples=%d filter_length=%d",
           aec_get_mode_string(kAecMode), s_aec_frame_samples, kAecFilterLength);
  return ESP_OK;
}

esp_err_t generate_aec_frame() {
  ESP_RETURN_ON_ERROR(read_stereo_capture(static_cast<size_t>(s_aec_frame_samples)),
                      TAG, "Failed to read AEC capture frame");
  for (int i = 0; i < s_aec_frame_samples; ++i) {
    s_aec_mic_buffer[i] =
        downsample_capture_slot(static_cast<size_t>(i), kAecMicSlot);
    s_aec_ref_buffer[i] =
        downsample_capture_slot(static_cast<size_t>(i), kAecRefSlot);
  }

  const size_t frame_samples = static_cast<size_t>(s_aec_frame_samples);
  aec_process(s_aec_handle, s_aec_mic_buffer, s_aec_ref_buffer,
              s_aec_output_buffer);
  memcpy(s_aec_fifo_buffer, s_aec_output_buffer,
         frame_samples * sizeof(int16_t));
  s_aec_fifo_offset = 0;
  s_aec_fifo_samples = frame_samples;
  s_last_ref_peak = peak_for_samples(s_aec_ref_buffer, frame_samples);
  s_last_aec_peak = peak_for_samples(s_aec_output_buffer, frame_samples);
  return ESP_OK;
}

esp_err_t read_aec_mic(int16_t *dest, size_t mono_sample_count) {
  size_t copied = 0;
  while (copied < mono_sample_count) {
    if (s_aec_fifo_samples == 0) {
      ESP_RETURN_ON_ERROR(generate_aec_frame(), TAG,
                          "Failed to generate AEC frame");
    }

    const size_t to_copy =
        std::min(mono_sample_count - copied, s_aec_fifo_samples);
    memcpy(dest + copied, s_aec_fifo_buffer + s_aec_fifo_offset,
           to_copy * sizeof(int16_t));
    copied += to_copy;
    s_aec_fifo_offset += to_copy;
    s_aec_fifo_samples -= to_copy;
  }
  return ESP_OK;
}

}  // namespace

const EchoPyramidBoardConfig kEchoPyramidBoardConfig = {
    .i2c_port = I2C_NUM_1,
    .i2c_sda = GPIO_NUM_38,
    .i2c_scl = GPIO_NUM_39,
    .backlight_i2c_port = I2C_NUM_0,
    .backlight_sda = GPIO_NUM_45,
    .backlight_scl = GPIO_NUM_0,
    .i2s_port = I2S_NUM_0,
    .i2s_bclk = GPIO_NUM_6,
    .i2s_lrck = GPIO_NUM_8,
    .i2s_dout = GPIO_NUM_7,
    .i2s_din = GPIO_NUM_5,
    .display_host = SPI3_HOST,
    .display_sclk = GPIO_NUM_15,
    .display_mosi = GPIO_NUM_21,
    .display_cs = GPIO_NUM_14,
    .display_dc = GPIO_NUM_42,
    .display_reset = GPIO_NUM_48,
    .display_width = 128,
    .display_height = 128,
    .display_gap_x = 0,
    .display_gap_y = 32,
    .user_button = GPIO_NUM_41,
    .backlight_addr = 0x30,
    .backlight_brightness = 0x7F,
    .io_expander_addr = 0x43,
    .speaker_enable_pin = 0,
    .si5351_addr = 0x60,
    .aw87559_addr = 0x5B,
    .touch_addr = 0x1A,
};

esp_err_t board_audio_init(void) {
  if (s_audio_initialized) {
    return ESP_OK;
  }

  gpio_config_t button_cfg = {};
  button_cfg.pin_bit_mask = 1ULL << kEchoPyramidBoardConfig.user_button;
  button_cfg.mode = GPIO_MODE_INPUT;
  button_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  button_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  button_cfg.intr_type = GPIO_INTR_DISABLE;
  ESP_RETURN_ON_ERROR(gpio_config(&button_cfg), TAG,
                      "Failed to initialize user button");
  s_last_user_button_level = gpio_get_level(kEchoPyramidBoardConfig.user_button);

  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = kEchoPyramidBoardConfig.i2c_port;
  bus_cfg.sda_io_num = kEchoPyramidBoardConfig.i2c_sda;
  bus_cfg.scl_io_num = kEchoPyramidBoardConfig.i2c_scl;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.intr_priority = 0;
  bus_cfg.trans_queue_depth = 0;
  bus_cfg.flags.enable_internal_pullup = 1;
  bus_cfg.flags.allow_pd = 0;
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG,
                      "Failed to create I2C bus");
  log_i2c_scan();
  ESP_RETURN_ON_ERROR(init_io_expander(), TAG,
                      "Failed to initialize speaker enable expander");
  ESP_RETURN_ON_ERROR(init_touch_controller(), TAG,
                      "Failed to initialize touch controller");
  if (s_touch_dev != nullptr) {
    ESP_RETURN_ON_ERROR(set_light_brightness(1, kDefaultLightBrightness), TAG,
                        "Failed to set RGB channel 1 brightness");
    ESP_RETURN_ON_ERROR(set_light_brightness(2, kDefaultLightBrightness), TAG,
                        "Failed to set RGB channel 2 brightness");
    render_lights_for_state(static_cast<uint32_t>(xTaskGetTickCount()));
    ESP_RETURN_ON_ERROR(flush_rgb_frames(), TAG,
                        "Failed to set initial RGB state");
    ESP_LOGI(TAG, "Echo Pyramid RGB initialized brightness=%u%%",
             kDefaultLightBrightness);
  }
  ESP_RETURN_ON_ERROR(init_clock_generator(), TAG,
                      "Failed to initialize clock generator");
  ESP_RETURN_ON_ERROR(init_amplifier(), TAG, "Failed to initialize amplifier");
  ESP_RETURN_ON_ERROR(init_i2s(), TAG, "Failed to initialize I2S");
  ESP_RETURN_ON_ERROR(init_codecs(), TAG, "Failed to initialize codecs");
  ESP_RETURN_ON_ERROR(init_aec(), TAG, "Failed to initialize AEC");
  ESP_RETURN_ON_ERROR(board_audio_set_output_enabled(true), TAG,
                      "Failed to enable speaker path");

  s_audio_initialized = true;
  ESP_LOGI(TAG, "Echo Pyramid audio initialized");
  return ESP_OK;
}

esp_err_t board_audio_read(void *dest, size_t size) {
  if (!s_audio_initialized || s_mic_dev == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (dest == nullptr || (size % sizeof(int16_t)) != 0) {
    return ESP_ERR_INVALID_ARG;
  }

  const size_t mono_sample_count = size / sizeof(int16_t);
  auto *mono = static_cast<int16_t *>(dest);
  esp_err_t err = ESP_OK;
  if (s_aec_handle != nullptr) {
    err = read_aec_mic(mono, mono_sample_count);
  } else {
    err = read_raw_mic(mono, mono_sample_count);
  }
  if (err != ESP_OK) {
    return err;
  }

  ++s_mic_read_count;
  if ((s_mic_read_count % 250) == 0) {
    ESP_LOGI(TAG,
             "Mic peaks raw=%ld ref=%ld aec=%ld enabled=%d slots=[%ld,%ld,%ld,%ld]",
             static_cast<long>(s_last_mic_peak),
             static_cast<long>(s_last_ref_peak),
             static_cast<long>(s_last_aec_peak),
             s_aec_handle != nullptr,
             static_cast<long>(s_last_slot_peaks[0]),
             static_cast<long>(s_last_slot_peaks[1]),
             static_cast<long>(s_last_slot_peaks[2]),
             static_cast<long>(s_last_slot_peaks[3]));
  }
  return ESP_OK;
}

esp_err_t board_audio_read_raw(void *dest, size_t size) {
  if (!s_audio_initialized || s_mic_dev == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (dest == nullptr || (size % sizeof(int16_t)) != 0) {
    return ESP_ERR_INVALID_ARG;
  }

  const size_t mono_sample_count = size / sizeof(int16_t);
  ESP_RETURN_ON_ERROR(read_raw_mic(static_cast<int16_t *>(dest),
                                   mono_sample_count),
                      TAG, "Failed to read raw mic channel");
  ++s_mic_read_count;
  if ((s_mic_read_count % 250) == 0) {
    ESP_LOGI(TAG,
             "Mic peaks raw=%ld ref=%ld aec=%ld enabled=%d source=raw slots=[%ld,%ld,%ld,%ld]",
             static_cast<long>(s_last_mic_peak),
             static_cast<long>(s_last_ref_peak),
             static_cast<long>(s_last_aec_peak),
             s_aec_handle != nullptr,
             static_cast<long>(s_last_slot_peaks[0]),
             static_cast<long>(s_last_slot_peaks[1]),
             static_cast<long>(s_last_slot_peaks[2]),
             static_cast<long>(s_last_slot_peaks[3]));
  }
  return ESP_OK;
}

esp_err_t board_audio_write(const void *data, size_t size) {
  if (!s_audio_initialized || s_tx_handle == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  if (data == nullptr || (size % sizeof(int16_t)) != 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_output_enabled) {
    return ESP_OK;
  }

  const size_t mono_sample_count = size / sizeof(int16_t);
  const size_t stereo_bytes =
      mono_sample_count * kPlaybackChannels * sizeof(int16_t);
  if (s_speaker_stereo_buffer_bytes < stereo_bytes) {
    auto *new_buffer = static_cast<int16_t *>(
        realloc(s_speaker_stereo_buffer, stereo_bytes));
    if (new_buffer == nullptr) {
      return ESP_ERR_NO_MEM;
    }
    s_speaker_stereo_buffer = new_buffer;
    s_speaker_stereo_buffer_bytes = stereo_bytes;
  }

  const auto *mono = static_cast<const int16_t *>(data);
  for (size_t i = 0; i < mono_sample_count; ++i) {
    s_speaker_stereo_buffer[(i * kPlaybackChannels) + 0] = mono[i];
    s_speaker_stereo_buffer[(i * kPlaybackChannels) + 1] = mono[i];
    for (size_t channel = 2; channel < kPlaybackChannels; ++channel) {
      s_speaker_stereo_buffer[(i * kPlaybackChannels) + channel] = 0;
    }
  }

  size_t bytes_written = 0;
  const esp_err_t err = i2s_channel_write(s_tx_handle, s_speaker_stereo_buffer,
                                          stereo_bytes, &bytes_written,
                                          portMAX_DELAY);
  if (err != ESP_OK) {
    return err;
  }
  if (bytes_written != stereo_bytes) {
    ESP_LOGW(TAG, "Short I2S playback write: %u/%u bytes",
             static_cast<unsigned>(bytes_written),
             static_cast<unsigned>(stereo_bytes));
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

esp_err_t board_audio_set_output_enabled(bool enabled) {
  s_output_enabled = enabled;
  ESP_RETURN_ON_ERROR(set_speaker_enable(enabled), TAG,
                      "Failed to update speaker enable path");
  if (s_amp_dev != nullptr) {
    ESP_RETURN_ON_ERROR(write_i2c_reg(s_amp_dev, kAw87559RegSysctrl,
                                      enabled ? 0x78 : 0x00),
                        TAG, "Failed to update amplifier state");
  }
  return ESP_OK;
}

esp_err_t board_audio_set_volume(uint8_t volume_percent) {
  if (volume_percent > kMaxSpeakerVolumePercent) {
    volume_percent = kMaxSpeakerVolumePercent;
  }

  s_speaker_volume_percent = volume_percent;
  if (!s_audio_initialized) {
    return ESP_OK;
  }
  return apply_speaker_volume();
}

uint8_t board_audio_get_volume(void) { return s_speaker_volume_percent; }

void board_audio_get_stats(BoardAudioStats *stats) {
  if (stats == nullptr) {
    return;
  }

  stats->read_count = s_mic_read_count;
  stats->mic_peak = s_last_mic_peak;
  stats->ref_peak = s_last_ref_peak;
  stats->aec_peak = s_last_aec_peak;
  stats->volume_percent = s_speaker_volume_percent;
  stats->output_enabled = s_output_enabled;
  stats->aec_enabled = s_aec_handle != nullptr;
  stats->aec_frame_samples = s_aec_frame_samples;
}

esp_err_t board_audio_run_loopback_test(uint32_t duration_ms) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  duration_ms = std::min<uint32_t>(std::max<uint32_t>(duration_ms, 500), 3000);
  const size_t sample_count =
      static_cast<size_t>((static_cast<uint64_t>(kSampleRate) * duration_ms) /
                          1000);
  const size_t total_bytes = sample_count * sizeof(int16_t);
  auto *capture = static_cast<int16_t *>(malloc(total_bytes));
  if (capture == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Starting local audio loopback capture duration_ms=%lu",
           static_cast<unsigned long>(duration_ms));
  board_status_set_state(STATUS_LISTENING, "Loopback rec");

  size_t offset_samples = 0;
  int32_t capture_peak = 0;
  while (offset_samples < sample_count) {
    const size_t chunk_samples =
        std::min(kSelfTestFrameSamples, sample_count - offset_samples);
    esp_err_t err =
        board_audio_read(capture + offset_samples,
                         chunk_samples * sizeof(int16_t));
    if (err != ESP_OK) {
      free(capture);
      return err;
    }
    for (size_t i = 0; i < chunk_samples; ++i) {
      int32_t sample = capture[offset_samples + i];
      if (sample < 0) {
        sample = -sample;
      }
      capture_peak = std::max(capture_peak, sample);
    }
    offset_samples += chunk_samples;
  }

  ESP_LOGI(TAG, "Local audio loopback playback capture_peak=%ld last_ref=%ld",
           static_cast<long>(capture_peak), static_cast<long>(s_last_ref_peak));
  board_status_set_state(STATUS_BOT_SPEAKING, "Loopback play");

  for (size_t offset = 0; offset < sample_count; offset += kSelfTestFrameSamples) {
    const size_t chunk_samples =
        std::min(kSelfTestFrameSamples, sample_count - offset);
    esp_err_t err =
        board_audio_write(capture + offset, chunk_samples * sizeof(int16_t));
    if (err != ESP_OK) {
      free(capture);
      return err;
    }
  }

  free(capture);
  board_status_set_state(STATUS_CONNECTED_IDLE, "Loopback done");
  ESP_LOGI(TAG, "Completed local audio loopback test");
  return ESP_OK;
}

esp_err_t board_audio_run_aec_probe(uint32_t duration_ms,
                                    uint32_t frequency_hz,
                                    float amplitude,
                                    BoardAudioAecProbeResult *result) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }
  if (result == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_aec_handle == nullptr || s_aec_frame_samples <= 0) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  duration_ms = std::min<uint32_t>(std::max<uint32_t>(duration_ms, 800), 6000);
  if (frequency_hz != 0) {
    frequency_hz =
        std::min<uint32_t>(std::max<uint32_t>(frequency_hz, 200), 3000);
  }
  amplitude = std::min<float>(std::max<float>(amplitude, 500.0f), 6000.0f);

  memset(result, 0, sizeof(*result));
  result->duration_ms = duration_ms;
  result->frequency_hz = frequency_hz;

  AecProbePlaybackContext ctx = {
      .duration_ms = duration_ms,
      .frequency_hz = frequency_hz,
      .amplitude = amplitude,
      .done = false,
      .result = ESP_OK,
  };

  ESP_LOGI(TAG,
           "Starting AEC probe duration_ms=%lu signal=%s frequency=%lu "
           "amplitude=%.0f",
           static_cast<unsigned long>(duration_ms),
           frequency_hz == 0 ? "noise" : "sine",
           static_cast<unsigned long>(frequency_hz), amplitude);
  board_status_set_state(STATUS_BOT_SPEAKING, "AEC probe");

  BaseType_t created = xTaskCreatePinnedToCore(
      aec_probe_playback_task, "aec_probe_play", 4096, &ctx, 7, nullptr, 1);
  if (created != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  vTaskDelay(pdMS_TO_TICKS(80));

  double mic_square_sum = 0.0;
  double ref_square_sum = 0.0;
  double aec_square_sum = 0.0;
  double mic_ref_sum = 0.0;
  double aec_ref_sum = 0.0;
  double slot_square_sum[kMicCaptureChannels] = {0.0};
  double slot_mic_sum[kMicCaptureChannels] = {0.0};
  uint64_t sample_count = 0;
  uint32_t frame_count = 0;
  int32_t mic_peak = 0;
  int32_t ref_peak = 0;
  int32_t aec_peak = 0;
  int32_t slot_peak[kMicCaptureChannels] = {0};

  while (!ctx.done) {
    const esp_err_t err = generate_aec_frame();
    if (err != ESP_OK) {
      ctx.result = err;
      break;
    }
    ++frame_count;
    if (frame_count <= kProbeSettlingFrames) {
      continue;
    }

    const size_t frame_samples = static_cast<size_t>(s_aec_frame_samples);
    for (size_t i = 0; i < frame_samples; ++i) {
      const float mic = static_cast<float>(s_aec_mic_buffer[i]);
      const float ref = static_cast<float>(s_aec_ref_buffer[i]);
      const float aec = static_cast<float>(s_aec_output_buffer[i]);
      mic_square_sum += static_cast<double>(mic * mic);
      ref_square_sum += static_cast<double>(ref * ref);
      aec_square_sum += static_cast<double>(aec * aec);
      mic_ref_sum += static_cast<double>(mic * ref);
      aec_ref_sum += static_cast<double>(aec * ref);
      for (size_t channel = 0; channel < kMicCaptureChannels; ++channel) {
        const float slot =
            static_cast<float>(downsample_capture_slot(i, channel));
        slot_square_sum[channel] += static_cast<double>(slot * slot);
        slot_mic_sum[channel] += static_cast<double>(slot * mic);
      }
      ++sample_count;
    }
    mic_peak = std::max(mic_peak, peak_for_samples(s_aec_mic_buffer, frame_samples));
    ref_peak = std::max(ref_peak, peak_for_samples(s_aec_ref_buffer, frame_samples));
    aec_peak = std::max(aec_peak, peak_for_samples(s_aec_output_buffer, frame_samples));
    for (size_t channel = 0; channel < kMicCaptureChannels; ++channel) {
      slot_peak[channel] = std::max(slot_peak[channel], s_last_slot_peaks[channel]);
    }
  }

  while (!ctx.done) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  if (sample_count > 0) {
    result->mic_rms = sqrt(mic_square_sum / static_cast<double>(sample_count));
    result->ref_rms = sqrt(ref_square_sum / static_cast<double>(sample_count));
    result->aec_rms = sqrt(aec_square_sum / static_cast<double>(sample_count));
    const double mic_ref_den = sqrt(mic_square_sum * ref_square_sum);
    const double aec_ref_den = sqrt(aec_square_sum * ref_square_sum);
    result->mic_ref_corr =
        mic_ref_den > 0.0 ? static_cast<float>(mic_ref_sum / mic_ref_den) : 0.0f;
    result->aec_ref_corr =
        aec_ref_den > 0.0 ? static_cast<float>(aec_ref_sum / aec_ref_den) : 0.0f;
    for (size_t channel = 0; channel < kMicCaptureChannels; ++channel) {
      result->slot_rms[channel] =
          sqrt(slot_square_sum[channel] / static_cast<double>(sample_count));
      const double slot_mic_den = sqrt(slot_square_sum[channel] * mic_square_sum);
      result->slot_corr[channel] =
          slot_mic_den > 0.0
              ? static_cast<float>(slot_mic_sum[channel] / slot_mic_den)
              : 0.0f;
    }
    if (result->aec_rms > 0.0f && result->mic_rms > 0.0f) {
      result->suppression_db = 20.0f * log10f(result->mic_rms / result->aec_rms);
    }
  }

  result->frames = frame_count;
  result->mic_peak = mic_peak;
  result->ref_peak = ref_peak;
  result->aec_peak = aec_peak;
  for (size_t channel = 0; channel < kMicCaptureChannels; ++channel) {
    result->slot_peaks[channel] = slot_peak[channel];
  }
  s_last_mic_peak = mic_peak;
  s_last_ref_peak = ref_peak;
  s_last_aec_peak = aec_peak;

  board_status_set_state(STATUS_CONNECTED_IDLE, "AEC done");
  ESP_LOGI(TAG,
           "AEC_PROBE result=%s signal=%s frames=%lu mic_peak=%ld ref_peak=%ld "
           "aec_peak=%ld mic_rms=%.1f ref_rms=%.1f aec_rms=%.1f "
           "mic_ref_corr=%.3f aec_ref_corr=%.3f suppression_db=%.2f "
           "slot_peaks=[%ld,%ld,%ld,%ld] slot_rms=[%.1f,%.1f,%.1f,%.1f] "
           "slot_corr=[%.3f,%.3f,%.3f,%.3f]",
           esp_err_to_name(ctx.result), frequency_hz == 0 ? "noise" : "sine",
           static_cast<unsigned long>(result->frames),
           static_cast<long>(result->mic_peak),
           static_cast<long>(result->ref_peak),
           static_cast<long>(result->aec_peak), result->mic_rms,
           result->ref_rms, result->aec_rms, result->mic_ref_corr,
           result->aec_ref_corr, result->suppression_db,
           static_cast<long>(result->slot_peaks[0]),
           static_cast<long>(result->slot_peaks[1]),
           static_cast<long>(result->slot_peaks[2]),
           static_cast<long>(result->slot_peaks[3]), result->slot_rms[0],
           result->slot_rms[1], result->slot_rms[2], result->slot_rms[3],
           result->slot_corr[0], result->slot_corr[1], result->slot_corr[2],
           result->slot_corr[3]);
  return ctx.result;
}

void board_audio_dump_diagnostics(void) {
  static const uint8_t kEs8311Registers[] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
      0x0D, 0x0E, 0x12, 0x13, 0x14, 0x16, 0x17, 0x1B, 0x1C, 0x31, 0x32,
      0x37, 0x44, 0x45,
  };
  static const uint8_t kEs7210Registers[] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,
      0x11, 0x12, 0x13, 0x20, 0x21, 0x22, 0x23, 0x40, 0x41, 0x42, 0x43,
      0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C,
  };

  ESP_LOGI(TAG, "Dumping Echo Pyramid codec diagnostics");
  dump_codec_registers("ES8311", s_speaker_dev, kEs8311Registers,
                       sizeof(kEs8311Registers));
  dump_codec_registers("ES7210", s_mic_dev, kEs7210Registers,
                       sizeof(kEs7210Registers));
  if (s_amp_dev != nullptr) {
    uint8_t sysctrl = 0;
    uint8_t pa_gain = 0;
    if (read_i2c_reg(s_amp_dev, kAw87559RegSysctrl, &sysctrl) == ESP_OK &&
        read_i2c_reg(s_amp_dev, kAw87559RegPaGain, &pa_gain) == ESP_OK) {
      ESP_LOGI(TAG, "AW87559 reg[0x%02x] = 0x%02x reg[0x%02x] = 0x%02x",
               kAw87559RegSysctrl, sysctrl, kAw87559RegPaGain, pa_gain);
    }
  }
}

esp_err_t board_audio_play_self_test(void) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!kSelfTestTonesEnabled) {
    ESP_LOGI(TAG, "Skipping local speaker self-test tone");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Starting local speaker self-test");
  ESP_RETURN_ON_ERROR(play_tone(440.0f, 250), TAG, "Failed 440Hz self-test");
  ESP_RETURN_ON_ERROR(play_silence(120), TAG, "Failed self-test silence");
  ESP_RETURN_ON_ERROR(play_tone(660.0f, 250), TAG, "Failed 660Hz self-test");
  ESP_RETURN_ON_ERROR(play_silence(120), TAG, "Failed self-test silence");
  ESP_RETURN_ON_ERROR(play_tone(880.0f, 250), TAG, "Failed 880Hz self-test");
  ESP_LOGI(TAG, "Completed local speaker self-test");
  vTaskDelay(pdMS_TO_TICKS(50));
  return ESP_OK;
}

esp_err_t board_audio_play_wake_tone(void) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!kInteractionTonesEnabled) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(play_tone(880.0f, 60), TAG, "Failed wake tone");
  ESP_RETURN_ON_ERROR(play_silence(20), TAG, "Failed wake tone silence");
  ESP_RETURN_ON_ERROR(play_tone(1320.0f, 80), TAG, "Failed wake tone");
  return ESP_OK;
}

esp_err_t board_audio_play_listening_tone(void) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!kInteractionTonesEnabled) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(play_tone_enveloped(523.25f, 90, 760.0f, 28, 42),
                      TAG, "Failed listening tone");
  ESP_RETURN_ON_ERROR(play_silence(20), TAG, "Failed listening tone silence");
  ESP_RETURN_ON_ERROR(play_tone_enveloped(659.25f, 130, 920.0f, 36, 70),
                      TAG, "Failed listening tone");
  return ESP_OK;
}

esp_err_t board_audio_play_thinking_tone(void) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!kInteractionTonesEnabled) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(play_tone(698.0f, 45), TAG, "Failed thinking tone");
  ESP_RETURN_ON_ERROR(play_silence(20), TAG, "Failed thinking tone silence");
  ESP_RETURN_ON_ERROR(play_tone(880.0f, 55), TAG, "Failed thinking tone");
  return ESP_OK;
}

esp_err_t board_audio_play_tool_tone(void) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!kInteractionTonesEnabled) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(play_tone(988.0f, 40), TAG, "Failed tool tone");
  ESP_RETURN_ON_ERROR(play_silence(20), TAG, "Failed tool tone silence");
  ESP_RETURN_ON_ERROR(play_tone(1175.0f, 50), TAG, "Failed tool tone");
  return ESP_OK;
}

esp_err_t board_audio_play_ready_tone(void) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!kInteractionTonesEnabled) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(play_tone(1046.0f, 90), TAG, "Failed ready tone");
  return ESP_OK;
}

esp_err_t board_audio_play_failure_tone(void) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!kInteractionTonesEnabled) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(play_tone(660.0f, 90), TAG, "Failed failure tone");
  ESP_RETURN_ON_ERROR(play_silence(25), TAG, "Failed failure tone silence");
  ESP_RETURN_ON_ERROR(play_tone(440.0f, 180), TAG, "Failed failure tone");
  return ESP_OK;
}

esp_err_t board_audio_play_timer_done_tone(void) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!kInteractionTonesEnabled) {
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(play_tone(1046.0f, 140), TAG, "Failed timer done tone");
  ESP_RETURN_ON_ERROR(play_silence(35), TAG, "Failed timer done tone silence");
  ESP_RETURN_ON_ERROR(play_tone(1318.0f, 180), TAG, "Failed timer done tone");
  ESP_RETURN_ON_ERROR(play_silence(35), TAG, "Failed timer done tone silence");
  ESP_RETURN_ON_ERROR(play_tone(1568.0f, 260), TAG, "Failed timer done tone");
  return ESP_OK;
}

esp_err_t board_audio_play_beep(uint32_t frequency_hz, uint32_t duration_ms) {
  if (!s_audio_initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  if (!kInteractionTonesEnabled) {
    return ESP_OK;
  }

  frequency_hz = std::max<uint32_t>(100, std::min<uint32_t>(frequency_hz, 5000));
  duration_ms = std::max<uint32_t>(20, std::min<uint32_t>(duration_ms, 1000));
  return play_tone(static_cast<float>(frequency_hz), duration_ms);
}

bool board_controls_poll(void) {
  const TickType_t now = xTaskGetTickCount();
  const int user_button_level = gpio_get_level(kEchoPyramidBoardConfig.user_button);
  if (s_last_user_button_level == 1 && user_button_level == 0 &&
      (now - s_last_user_button_toggle) > pdMS_TO_TICKS(250)) {
    s_talk_enabled = !s_talk_enabled;
    s_last_user_button_toggle = now;
    hermes_media_set_publish_enabled(s_talk_enabled);
    board_status_set_talk_enabled(s_talk_enabled);
    ESP_LOGI(TAG, "Talk publishing %s", s_talk_enabled ? "enabled" : "muted");
  }
  s_last_user_button_level = user_button_level;

  if (s_touch_dev == nullptr) {
    return false;
  }

  uint8_t touch_state = 0;
  if (read_touch_state(&touch_state) != ESP_OK) {
    return false;
  }

  const uint8_t new_presses =
      touch_state & static_cast<uint8_t>(~s_last_touch_state);
  s_last_touch_state = touch_state;

  if ((touch_state & 0x0F) == 0) {
    s_swipe_first_touch = 0;
  } else if (s_swipe_first_touch != 0 &&
             static_cast<int32_t>(now - s_swipe_deadline) > 0) {
    s_swipe_first_touch = 0;
  }

  if ((new_presses & 0x01) && s_swipe_first_touch == 0) {
    board_status_set_state(STATUS_LISTENING, "Touch");
    board_audio_play_wake_tone();
    return true;
  }

  if (s_swipe_first_touch == 0) {
    if (new_presses & 0x04) {
      s_swipe_first_touch = 3;
      s_swipe_deadline = now + kSwipeTimeoutTicks;
    } else if (new_presses & 0x08) {
      s_swipe_first_touch = 4;
      s_swipe_deadline = now + kSwipeTimeoutTicks;
    } else if (new_presses & 0x01) {
      s_swipe_first_touch = 1;
      s_swipe_deadline = now + kSwipeTimeoutTicks;
    } else if (new_presses & 0x02) {
      s_swipe_first_touch = 2;
      s_swipe_deadline = now + kSwipeTimeoutTicks;
    }
    return false;
  }

  uint8_t next_volume = s_speaker_volume_percent;
  bool volume_changed = false;

  const bool swipe_up =
      (s_swipe_first_touch == 2 && (new_presses & 0x01)) ||
      (s_swipe_first_touch == 4 && (new_presses & 0x04));
  const bool swipe_down =
      (s_swipe_first_touch == 1 && (new_presses & 0x02)) ||
      (s_swipe_first_touch == 3 && (new_presses & 0x08));

  if (swipe_up) {
    next_volume = static_cast<uint8_t>(
        next_volume + kSpeakerVolumeStepPercent > kMaxSpeakerVolumePercent
            ? kMaxSpeakerVolumePercent
            : next_volume + kSpeakerVolumeStepPercent);
    volume_changed = next_volume != s_speaker_volume_percent;
    s_swipe_first_touch = 0;
  } else if (swipe_down) {
    next_volume =
        (next_volume >= kSpeakerVolumeStepPercent)
            ? static_cast<uint8_t>(next_volume - kSpeakerVolumeStepPercent)
            : 0;
    volume_changed = next_volume != s_speaker_volume_percent;
    s_swipe_first_touch = 0;
  }

  if (volume_changed && board_audio_set_volume(next_volume) == ESP_OK) {
    ESP_LOGI(TAG, "Touch volume set to %u%%", next_volume);
    board_status_show_volume(next_volume);
    hermes_webrtc_send_volume(next_volume);
  }

  return false;
}

void board_dance_mode_set(bool enabled) {
  s_dance_mode = enabled;
  if (enabled) {
    s_custom_lights_enabled = false;
  }
}

bool board_dance_mode_active(void) { return s_dance_mode; }

void board_lights_set_state(StatusState state) { s_light_state = state; }

void board_lights_set_effect(const char *color, const char *pattern) {
  const char *effective_color = color != nullptr ? color : "white";
  const char *effective_pattern = pattern != nullptr ? pattern : "solid";

  if (strcmp(effective_pattern, "dance") == 0 ||
      strcmp(effective_pattern, "cleanup") == 0 ||
      strcmp(effective_pattern, "rainbow") == 0) {
    board_dance_mode_set(true);
    return;
  }
  if (strcmp(effective_pattern, "off") == 0 ||
      strcmp(effective_color, "off") == 0 ||
      strcmp(effective_color, "black") == 0) {
    s_dance_mode = false;
    s_custom_lights_enabled = false;
    return;
  }

  s_dance_mode = false;
  s_custom_lights_enabled = true;
  if (strcmp(effective_color, "green") == 0) {
    s_custom_red = 0;
    s_custom_green = 180;
    s_custom_blue = 40;
  } else if (strcmp(effective_color, "blue") == 0) {
    s_custom_red = 20;
    s_custom_green = 70;
    s_custom_blue = 220;
  } else if (strcmp(effective_color, "amber") == 0 ||
             strcmp(effective_color, "yellow") == 0) {
    s_custom_red = 220;
    s_custom_green = 130;
    s_custom_blue = 0;
  } else if (strcmp(effective_color, "red") == 0) {
    s_custom_red = 220;
    s_custom_green = 0;
    s_custom_blue = 0;
  } else if (strcmp(effective_color, "purple") == 0) {
    s_custom_red = 120;
    s_custom_green = 20;
    s_custom_blue = 200;
  } else {
    s_custom_red = 160;
    s_custom_green = 160;
    s_custom_blue = 160;
  }
  strncpy(s_custom_pattern, effective_pattern, sizeof(s_custom_pattern) - 1);
  s_custom_pattern[sizeof(s_custom_pattern) - 1] = '\0';
}

void board_lights_tick(void) {
  if (s_touch_dev == nullptr) {
    return;
  }

  const TickType_t now = xTaskGetTickCount();
  if ((now - s_last_lights_tick) < kLightsTickInterval) {
    return;
  }
  s_last_lights_tick = now;

  render_lights_for_state(static_cast<uint32_t>(now));
  flush_rgb_frames();
}
