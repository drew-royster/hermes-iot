#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_aec.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char *TAG = "aec_lab";

constexpr i2c_port_num_t kI2cPort = I2C_NUM_1;
constexpr gpio_num_t kI2cSda = GPIO_NUM_38;
constexpr gpio_num_t kI2cScl = GPIO_NUM_39;
constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr gpio_num_t kI2sBclk = GPIO_NUM_6;
constexpr gpio_num_t kI2sLrck = GPIO_NUM_8;
constexpr gpio_num_t kI2sDout = GPIO_NUM_7;
constexpr gpio_num_t kI2sDin = GPIO_NUM_5;

constexpr uint8_t kAddrEs8311 = 0x18;
constexpr uint8_t kAddrStm32 = 0x1a;
constexpr uint8_t kAddrEs7210 = 0x40;
constexpr uint8_t kAddrIoExpander = 0x43;
constexpr uint8_t kAddrAw87559 = 0x5b;
constexpr uint8_t kAddrSi5351 = 0x60;

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kVoiceRate = 16000;
constexpr size_t kDownsample = kSampleRate / kVoiceRate;
constexpr size_t kFrameSamples = 240;
constexpr size_t kProbeMs = 3000;
constexpr size_t kProbeSettleFrames = 20;
constexpr float kProbeAmplitude = 3500.0f;

i2c_master_bus_handle_t s_i2c_bus = nullptr;
i2s_chan_handle_t s_tx = nullptr;
i2s_chan_handle_t s_rx = nullptr;

struct CachedDev {
  uint8_t addr;
  i2c_master_dev_handle_t handle;
};

CachedDev s_devs[8] = {};
size_t s_dev_count = 0;

struct PlaybackContext {
  uint32_t seed;
  size_t frames;
  size_t channels;
  volatile bool done;
  esp_err_t result;
  size_t last_written;
};

struct ProbeMetrics {
  uint32_t frames = 0;
  uint64_t samples = 0;
  int32_t slot_peak[4] = {};
  float slot_rms[4] = {};
  float slot_corr[4] = {};
  int32_t aec_peak = 0;
  float aec_rms = 0.0f;
  float aec_ref_corr = 0.0f;
  float suppression_db = 0.0f;
};

struct LabCase {
  const char *name;
  bool rx_tdm;
  uint8_t es7210_reg12;
  uint8_t es7210_reg4b;
  uint8_t es7210_reg4c;
  uint8_t mic1_gain;
  uint8_t mic3_gain;
  bool write_amp_gain;
  uint8_t amp_gain;
  uint8_t amp_sysctrl;
  uint8_t dac_volume;
  int aec_filter_length;
  aec_mode_t aec_mode;
};

const LabCase kCases[] = {
    {
        .name = "std_vendor_ref_amp_off",
        .rx_tdm = false,
        .es7210_reg12 = 0x00,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x00,
        .dac_volume = 60,
        .aec_filter_length = 4,
        .aec_mode = AEC_MODE_VOIP_HIGH_PERF,
    },
    {
        .name = "std_vendor_sys78_voip_high",
        .rx_tdm = false,
        .es7210_reg12 = 0x00,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x78,
        .dac_volume = 60,
        .aec_filter_length = 4,
        .aec_mode = AEC_MODE_VOIP_HIGH_PERF,
    },
    {
        .name = "std_reg12_02_sys78_voip_high",
        .rx_tdm = false,
        .es7210_reg12 = 0x02,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x78,
        .dac_volume = 60,
        .aec_filter_length = 4,
        .aec_mode = AEC_MODE_VOIP_HIGH_PERF,
    },
    {
        .name = "tdm_ref_amp_off",
        .rx_tdm = true,
        .es7210_reg12 = 0x02,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x00,
        .dac_volume = 60,
        .aec_filter_length = 4,
        .aec_mode = AEC_MODE_SR_LOW_COST,
    },
    {
        .name = "tdm_sys78_aec4_sr_low",
        .rx_tdm = true,
        .es7210_reg12 = 0x02,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x78,
        .dac_volume = 60,
        .aec_filter_length = 4,
        .aec_mode = AEC_MODE_SR_LOW_COST,
    },
    {
        .name = "tdm_sys78_aec8_sr_low",
        .rx_tdm = true,
        .es7210_reg12 = 0x02,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x78,
        .dac_volume = 60,
        .aec_filter_length = 8,
        .aec_mode = AEC_MODE_SR_LOW_COST,
    },
    {
        .name = "tdm_sys78_aec12_sr_low",
        .rx_tdm = true,
        .es7210_reg12 = 0x02,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x78,
        .dac_volume = 60,
        .aec_filter_length = 12,
        .aec_mode = AEC_MODE_SR_LOW_COST,
    },
    {
        .name = "tdm_sys78_aec4_sr_high",
        .rx_tdm = true,
        .es7210_reg12 = 0x02,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x78,
        .dac_volume = 60,
        .aec_filter_length = 4,
        .aec_mode = AEC_MODE_SR_HIGH_PERF,
    },
    {
        .name = "tdm_sys78_aec8_sr_high",
        .rx_tdm = true,
        .es7210_reg12 = 0x02,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x78,
        .dac_volume = 60,
        .aec_filter_length = 8,
        .aec_mode = AEC_MODE_SR_HIGH_PERF,
    },
    {
        .name = "tdm_sys78_aec4_voip_high",
        .rx_tdm = true,
        .es7210_reg12 = 0x02,
        .es7210_reg4b = 0x0f,
        .es7210_reg4c = 0x0f,
        .mic1_gain = 0x18,
        .mic3_gain = 0x1e,
        .write_amp_gain = false,
        .amp_gain = 0x00,
        .amp_sysctrl = 0x78,
        .dac_volume = 60,
        .aec_filter_length = 4,
        .aec_mode = AEC_MODE_VOIP_HIGH_PERF,
    },
};

i2c_master_dev_handle_t get_dev(uint8_t addr) {
  for (size_t i = 0; i < s_dev_count; ++i) {
    if (s_devs[i].addr == addr) {
      return s_devs[i].handle;
    }
  }
  if (s_dev_count >= sizeof(s_devs) / sizeof(s_devs[0])) {
    return nullptr;
  }
  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.device_address = addr;
  dev_cfg.scl_speed_hz = 400000;
  i2c_master_dev_handle_t handle = nullptr;
  if (i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &handle) != ESP_OK) {
    return nullptr;
  }
  s_devs[s_dev_count++] = {.addr = addr, .handle = handle};
  return handle;
}

esp_err_t write_reg(uint8_t addr, uint8_t reg, uint8_t value) {
  i2c_master_dev_handle_t dev = get_dev(addr);
  if (dev == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  uint8_t payload[2] = {reg, value};
  return i2c_master_transmit(dev, payload, sizeof(payload), pdMS_TO_TICKS(100));
}

esp_err_t read_reg(uint8_t addr, uint8_t reg, uint8_t *value) {
  i2c_master_dev_handle_t dev = get_dev(addr);
  if (dev == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  return i2c_master_transmit_receive(dev, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

esp_err_t write_bulk(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len) {
  i2c_master_dev_handle_t dev = get_dev(addr);
  if (dev == nullptr || len > 63) {
    return ESP_ERR_INVALID_ARG;
  }
  uint8_t payload[64] = {};
  payload[0] = reg;
  memcpy(payload + 1, data, len);
  return i2c_master_transmit(dev, payload, len + 1, pdMS_TO_TICKS(100));
}

esp_err_t update_bits(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = 0;
  ESP_RETURN_ON_ERROR(read_reg(addr, reg, &current), TAG, "read bits failed");
  current = static_cast<uint8_t>((current & ~mask) | (value & mask));
  return write_reg(addr, reg, current);
}

esp_err_t init_i2c() {
  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = kI2cPort;
  bus_cfg.sda_io_num = kI2cSda;
  bus_cfg.scl_io_num = kI2cScl;
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;
  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG,
                      "i2c bus init failed");
  for (uint8_t addr : {kAddrEs8311, kAddrStm32, kAddrEs7210, kAddrAw87559,
                       kAddrSi5351}) {
    uint8_t value = 0;
    esp_err_t err = read_reg(addr, 0x00, &value);
    ESP_LOGI(TAG, "I2C probe addr=0x%02x result=%s reg00=0x%02x", addr,
             esp_err_to_name(err), value);
  }
  return ESP_OK;
}

esp_err_t si5351_init_48k() {
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 3, 0xff), TAG, "si5351 off");
  vTaskDelay(pdMS_TO_TICKS(10));
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 16, 0x80), TAG, "si5351 clk0 off");
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 17, 0x80), TAG, "si5351 clk1 off");
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 18, 0x80), TAG, "si5351 clk2 off");
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 183, 0xc0), TAG, "si5351 cap");

  // M5 reference: 12.288 MHz = 884.736 MHz PLL / 72 on CLK1.
  const uint32_t pll_freq = 884736000UL;
  const uint32_t xtal = 27000000UL;
  const uint32_t a = pll_freq / xtal;
  const uint32_t rest = pll_freq % xtal;
  const uint32_t c = 1000000UL;
  const uint32_t b = (rest * c) / xtal;
  const uint32_t p1 = 128 * a + (128 * b) / c - 512;
  const uint32_t p2 = 128 * b - c * ((128 * b) / c);
  const uint32_t p3 = c;
  uint8_t pll_buf[8] = {
      static_cast<uint8_t>((p3 >> 8) & 0xff),
      static_cast<uint8_t>(p3 & 0xff),
      static_cast<uint8_t>((p1 >> 16) & 0x03),
      static_cast<uint8_t>((p1 >> 8) & 0xff),
      static_cast<uint8_t>(p1 & 0xff),
      static_cast<uint8_t>(((p3 >> 12) & 0xf0) | ((p2 >> 16) & 0x0f)),
      static_cast<uint8_t>((p2 >> 8) & 0xff),
      static_cast<uint8_t>(p2 & 0xff),
  };
  ESP_RETURN_ON_ERROR(write_bulk(kAddrSi5351, 26, pll_buf, sizeof(pll_buf)), TAG,
                      "si5351 pll");

  const uint32_t ms_div = 72;
  const uint32_t ms_p1 = 128 * ms_div - 512;
  uint8_t ms_buf[8] = {
      0x00,
      0x01,
      static_cast<uint8_t>((ms_p1 >> 16) & 0x03),
      static_cast<uint8_t>((ms_p1 >> 8) & 0xff),
      static_cast<uint8_t>(ms_p1 & 0xff),
      0x00,
      0x00,
      0x00,
  };
  ESP_RETURN_ON_ERROR(write_bulk(kAddrSi5351, 50, ms_buf, sizeof(ms_buf)), TAG,
                      "si5351 ms1");
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 17, 0x4f), TAG, "si5351 clk1");
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 16, 0x80), TAG, "si5351 clk0 off2");
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 18, 0x80), TAG, "si5351 clk2 off2");
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 177, 0xa0), TAG, "si5351 reset");
  vTaskDelay(pdMS_TO_TICKS(10));
  ESP_RETURN_ON_ERROR(write_reg(kAddrSi5351, 3, 0xfd), TAG, "si5351 enable");
  ESP_LOGI(TAG, "Si5351 CLK1 set to 12.288 MHz");
  return ESP_OK;
}

esp_err_t es8311_set_volume(uint8_t volume) {
  volume = std::min<uint8_t>(volume, 100);
  const uint8_t reg = volume == 0 ? 0 : static_cast<uint8_t>(((volume * 256) / 100) - 1);
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x34, 0x80), TAG, "es8311 vol34");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x35, 0x90), TAG, "es8311 vol35");
  return write_reg(kAddrEs8311, 0x32, reg);
}

esp_err_t es8311_init_m5(uint8_t volume) {
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x00, 0x1f), TAG, "es8311 reset1");
  vTaskDelay(pdMS_TO_TICKS(20));
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x00, 0x00), TAG, "es8311 reset2");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x00, 0x80), TAG, "es8311 reset3");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x01, 0x3f), TAG, "es8311 clock");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x02, 0x00), TAG, "es8311 coeff2");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x03, 0x10), TAG, "es8311 coeff3");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x04, 0x20), TAG, "es8311 coeff4");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x05, 0x00), TAG, "es8311 coeff5");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x06, 0x00), TAG, "es8311 coeff6");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x07, 0x00), TAG, "es8311 coeff7");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x08, 0xff), TAG, "es8311 coeff8");
  uint8_t reg00 = 0;
  ESP_RETURN_ON_ERROR(read_reg(kAddrEs8311, 0x00, &reg00), TAG, "es8311 read00");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x00, reg00 & ~(1 << 6)), TAG,
                      "es8311 slave");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x09, 0x0c), TAG, "es8311 i2sin");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x0a, 0x0c), TAG, "es8311 i2sout");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x0d, 0x01), TAG, "es8311 sys0d");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x0e, 0x02), TAG, "es8311 sys0e");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x12, 0x00), TAG, "es8311 sys12");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x13, 0x10), TAG, "es8311 sys13");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x1c, 0x6a), TAG, "es8311 adc");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs8311, 0x37, 0x08), TAG, "es8311 dac");
  ESP_RETURN_ON_ERROR(es8311_set_volume(volume), TAG, "es8311 volume");
  return ESP_OK;
}

esp_err_t es7210_init_profile(const LabCase &lab) {
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x00, 0xff), TAG, "es7210 reset1");
  vTaskDelay(pdMS_TO_TICKS(20));
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x00, 0x32), TAG, "es7210 reset2");
  vTaskDelay(pdMS_TO_TICKS(20));
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x01, 0x3f), TAG, "es7210 clocks off");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x03, 0x04), TAG, "es7210 mclk");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x04, 0x01), TAG, "es7210 lrckh");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x05, 0x00), TAG, "es7210 lrckl");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x02, 0x01), TAG, "es7210 mainclk");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x07, 0x20), TAG, "es7210 osr");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x08, 0x20), TAG, "es7210 mode");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x11, 0x60), TAG, "es7210 fmt");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x12, lab.es7210_reg12), TAG,
                      "es7210 tdm");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x40, 0x42), TAG, "es7210 analog");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x41, 0x70), TAG, "es7210 bias12");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x42, 0x70), TAG, "es7210 bias34");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x43, lab.mic1_gain), TAG,
                      "es7210 mic1 gain");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x44, 0x10), TAG, "es7210 mic2 gain");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x45, lab.mic3_gain), TAG,
                      "es7210 mic3 gain");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x46, 0x10), TAG, "es7210 mic4 gain");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x47, 0x08), TAG, "es7210 pwr1");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x48, 0x08), TAG, "es7210 pwr2");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x49, 0x08), TAG, "es7210 pwr3");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x4a, 0x08), TAG, "es7210 pwr4");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x4b, lab.es7210_reg4b), TAG,
                      "es7210 pwr12");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x4c, lab.es7210_reg4c), TAG,
                      "es7210 pwr34");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x13, 0x00), TAG, "es7210 automute");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x06, 0x00), TAG, "es7210 power");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x01, 0x00), TAG, "es7210 clocks on");
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x00, 0x71), TAG, "es7210 start1");
  vTaskDelay(pdMS_TO_TICKS(100));
  ESP_RETURN_ON_ERROR(write_reg(kAddrEs7210, 0x00, 0x41), TAG, "es7210 start2");
  vTaskDelay(pdMS_TO_TICKS(50));
  return ESP_OK;
}

esp_err_t amp_apply(uint8_t sysctrl, bool write_gain, uint8_t gain) {
  ESP_RETURN_ON_ERROR(write_reg(kAddrAw87559, 0x00, 0xff), TAG, "amp probe");
  if (write_gain) {
    ESP_RETURN_ON_ERROR(write_reg(kAddrAw87559, 0x05, gain), TAG, "amp gain");
  }
  return write_reg(kAddrAw87559, 0x01, sysctrl);
}

esp_err_t speaker_gate_set_enabled(bool enabled) {
  if (i2c_master_probe(s_i2c_bus, kAddrIoExpander, 20) != ESP_OK) {
    ESP_LOGW(TAG, "speaker enable expander 0x43 not present");
    return ESP_OK;
  }
  constexpr uint8_t speaker_mask = 0x01;
  ESP_RETURN_ON_ERROR(update_bits(kAddrIoExpander, 0x07, speaker_mask, 0x00),
                      TAG, "speaker gate high-z");
  ESP_RETURN_ON_ERROR(update_bits(kAddrIoExpander, 0x03, speaker_mask,
                                  speaker_mask),
                      TAG, "speaker gate output");
  ESP_RETURN_ON_ERROR(update_bits(kAddrIoExpander, 0x05, speaker_mask,
                                  enabled ? speaker_mask : 0x00),
                      TAG, "speaker gate enable");
  uint8_t out = 0;
  read_reg(kAddrIoExpander, 0x05, &out);
  ESP_LOGI(TAG, "speaker gate enabled=%d reg05=0x%02x", enabled, out);
  return ESP_OK;
}

void dump_regs(const char *name, uint8_t addr, const uint8_t *regs, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint8_t value = 0;
    if (read_reg(addr, regs[i], &value) == ESP_OK) {
      ESP_LOGI(TAG, "%s reg[0x%02x]=0x%02x", name, regs[i], value);
    }
  }
}

esp_err_t init_i2s_std() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(kI2sPort, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = kFrameSamples;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG,
                      "new std channel");
  i2s_std_config_t cfg = {};
  cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                     I2S_SLOT_MODE_STEREO);
  cfg.gpio_cfg.mclk = GPIO_NUM_NC;
  cfg.gpio_cfg.bclk = kI2sBclk;
  cfg.gpio_cfg.ws = kI2sLrck;
  cfg.gpio_cfg.dout = kI2sDout;
  cfg.gpio_cfg.din = kI2sDin;
  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &cfg), TAG, "tx std");
  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &cfg), TAG, "rx std");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx enable");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");
  return ESP_OK;
}

esp_err_t init_i2s_tdm_rx() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(kI2sPort, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = kFrameSamples;
  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG,
                      "new tdm channel");
  i2s_tdm_config_t cfg = {};
  cfg.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(kSampleRate);
  cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
      static_cast<i2s_tdm_slot_mask_t>(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                       I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
  cfg.slot_cfg.total_slot = 4;
  cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
  cfg.gpio_cfg.mclk = GPIO_NUM_NC;
  cfg.gpio_cfg.bclk = kI2sBclk;
  cfg.gpio_cfg.ws = kI2sLrck;
  cfg.gpio_cfg.dout = kI2sDout;
  cfg.gpio_cfg.din = kI2sDin;

  ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_tx, &cfg), TAG, "tx tdm");
  ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx, &cfg), TAG, "rx tdm");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "rx enable");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "tx enable");
  return ESP_OK;
}

void deinit_i2s() {
  if (s_tx != nullptr) {
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
  }
  if (s_rx != nullptr) {
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_rx);
    s_rx = nullptr;
  }
}

int16_t noise_sample(uint32_t *state, float amplitude) {
  *state = (*state * 1664525U) + 1013904223U;
  const int32_t centered = static_cast<int32_t>((*state >> 16) & 0xffffU) - 32768;
  return static_cast<int16_t>(centered * amplitude / 32768.0f);
}

void playback_task(void *arg) {
  auto *ctx = static_cast<PlaybackContext *>(arg);
  int16_t out[kFrameSamples * 4] = {};
  uint32_t seed = ctx->seed;
  for (size_t frame = 0; frame < ctx->frames; ++frame) {
    memset(out, 0, sizeof(out));
    for (size_t i = 0; i < kFrameSamples; ++i) {
      const int16_t sample = noise_sample(&seed, kProbeAmplitude);
      out[i * ctx->channels] = sample;
      if (ctx->channels > 1) {
        out[i * ctx->channels + 1] = sample;
      }
    }
    size_t written = 0;
    const size_t bytes_to_write = kFrameSamples * ctx->channels * sizeof(int16_t);
    ctx->result = i2s_channel_write(s_tx, out, bytes_to_write, &written,
                                    pdMS_TO_TICKS(1000));
    ctx->last_written = written;
    if (ctx->result != ESP_OK || written != bytes_to_write) {
      ctx->result = ctx->result == ESP_OK ? ESP_ERR_TIMEOUT : ctx->result;
      break;
    }
  }
  ctx->done = true;
  vTaskDelete(nullptr);
}

int16_t downsample_slot(const int16_t *samples, size_t frame, size_t channel,
                        size_t channels) {
  int32_t sum = 0;
  for (size_t j = 0; j < kDownsample; ++j) {
    sum += samples[((frame * kDownsample + j) * channels) + channel];
  }
  return static_cast<int16_t>(sum / static_cast<int32_t>(kDownsample));
}

int32_t abs16(int16_t value) {
  return value < 0 ? -static_cast<int32_t>(value) : static_cast<int32_t>(value);
}

esp_err_t run_probe(const LabCase &lab, ProbeMetrics *metrics) {
  memset(metrics, 0, sizeof(*metrics));
  const size_t channels = lab.rx_tdm ? 4 : 2;
  const size_t playback_frames = kProbeMs / (1000 / (kSampleRate / kFrameSamples));
  PlaybackContext ctx = {
      .seed = 0x2468ace0,
      .frames = playback_frames,
      .channels = channels,
      .done = false,
      .result = ESP_OK,
      .last_written = 0,
  };

  aec_handle_t *aec =
      aec_create(kVoiceRate, lab.aec_filter_length, 1, lab.aec_mode);
  if (aec == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  const int aec_frame_samples = aec_get_chunksize(aec);
  int16_t *aec_mic = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, aec_frame_samples * sizeof(int16_t), MALLOC_CAP_8BIT));
  int16_t *aec_ref = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, aec_frame_samples * sizeof(int16_t), MALLOC_CAP_8BIT));
  int16_t *aec_out = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, aec_frame_samples * sizeof(int16_t), MALLOC_CAP_8BIT));
  int16_t *rx = static_cast<int16_t *>(
      heap_caps_malloc(aec_frame_samples * kDownsample * channels * sizeof(int16_t),
                       MALLOC_CAP_8BIT));
  if (aec_mic == nullptr || aec_ref == nullptr || aec_out == nullptr ||
      rx == nullptr) {
    free(aec_mic);
    free(aec_ref);
    free(aec_out);
    free(rx);
    aec_destroy(aec);
    return ESP_ERR_NO_MEM;
  }

  BaseType_t created =
      xTaskCreatePinnedToCore(playback_task, "lab_playback", 4096, &ctx, 8,
                              nullptr, 1);
  if (created != pdPASS) {
    free(aec_mic);
    free(aec_ref);
    free(aec_out);
    free(rx);
    aec_destroy(aec);
    return ESP_ERR_NO_MEM;
  }
  vTaskDelay(pdMS_TO_TICKS(100));

  double slot_sq[4] = {};
  double slot_mic[4] = {};
  double aec_sq = 0.0;
  double aec_ref_dot = 0.0;
  double ref_sq = 0.0;
  uint32_t frame_count = 0;

  while (!ctx.done) {
    size_t bytes_read = 0;
    const size_t read_bytes =
        aec_frame_samples * kDownsample * channels * sizeof(int16_t);
    esp_err_t err = i2s_channel_read(s_rx, rx, read_bytes, &bytes_read,
                                     pdMS_TO_TICKS(1000));
    if (err != ESP_OK || bytes_read != read_bytes) {
      ctx.result = err == ESP_OK ? ESP_ERR_TIMEOUT : err;
      break;
    }
    ++frame_count;

    for (int i = 0; i < aec_frame_samples; ++i) {
      aec_mic[i] = downsample_slot(rx, i, 0, channels);
      aec_ref[i] = channels > 1 ? downsample_slot(rx, i, 1, channels) : 0;
      for (size_t ch = 0; ch < channels && ch < 4; ++ch) {
        const int16_t value = downsample_slot(rx, i, ch, channels);
        metrics->slot_peak[ch] = std::max(metrics->slot_peak[ch], abs16(value));
        slot_sq[ch] += static_cast<double>(value) * value;
        slot_mic[ch] += static_cast<double>(value) * aec_mic[i];
      }
    }
    aec_process(aec, aec_mic, aec_ref, aec_out);
    if (frame_count > kProbeSettleFrames) {
      for (int i = 0; i < aec_frame_samples; ++i) {
        metrics->aec_peak = std::max(metrics->aec_peak, abs16(aec_out[i]));
        aec_sq += static_cast<double>(aec_out[i]) * aec_out[i];
        ref_sq += static_cast<double>(aec_ref[i]) * aec_ref[i];
        aec_ref_dot += static_cast<double>(aec_out[i]) * aec_ref[i];
        ++metrics->samples;
      }
    }
  }

  while (!ctx.done) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (ctx.result != ESP_OK) {
    ESP_LOGW(TAG, "playback/read stopped result=%s last_written=%u",
             esp_err_to_name(ctx.result), static_cast<unsigned>(ctx.last_written));
  }

  metrics->frames = frame_count;
  const double mic_sq = slot_sq[0];
  for (size_t ch = 0; ch < channels && ch < 4; ++ch) {
    metrics->slot_rms[ch] =
        metrics->samples > 0
            ? sqrt(slot_sq[ch] / static_cast<double>(metrics->samples + kProbeSettleFrames * aec_frame_samples))
            : 0.0f;
    const double den = sqrt(slot_sq[ch] * mic_sq);
    metrics->slot_corr[ch] = den > 0.0 ? static_cast<float>(slot_mic[ch] / den) : 0.0f;
  }
  metrics->aec_rms =
      metrics->samples > 0 ? sqrt(aec_sq / static_cast<double>(metrics->samples)) : 0.0f;
  const double aec_ref_den = sqrt(aec_sq * ref_sq);
  metrics->aec_ref_corr =
      aec_ref_den > 0.0 ? static_cast<float>(aec_ref_dot / aec_ref_den) : 0.0f;
  if (metrics->slot_rms[0] > 0.0f && metrics->aec_rms > 0.0f) {
    metrics->suppression_db = 20.0f * log10f(metrics->slot_rms[0] / metrics->aec_rms);
  }

  free(aec_mic);
  free(aec_ref);
  free(aec_out);
  free(rx);
  aec_destroy(aec);
  return ctx.result;
}

esp_err_t run_case(const LabCase &lab) {
  ESP_LOGI(TAG,
           "LAB_CASE_BEGIN name=%s rx=%s reg12=0x%02x reg4b=0x%02x "
           "reg4c=0x%02x mic1=0x%02x mic3=0x%02x amp_sys=0x%02x "
           "amp_gain=%s0x%02x vol=%u aec_filter=%d aec_mode=%d",
           lab.name, lab.rx_tdm ? "tdm4" : "std2", lab.es7210_reg12,
           lab.es7210_reg4b, lab.es7210_reg4c, lab.mic1_gain, lab.mic3_gain,
           lab.amp_sysctrl, lab.write_amp_gain ? "" : "skip:",
           lab.amp_gain, lab.dac_volume, lab.aec_filter_length,
           static_cast<int>(lab.aec_mode));

  ESP_RETURN_ON_ERROR(amp_apply(0x00, false, 0x00), TAG, "amp mute");
  ESP_RETURN_ON_ERROR(speaker_gate_set_enabled(false), TAG, "speaker gate mute");
  ESP_RETURN_ON_ERROR(si5351_init_48k(), TAG, "si5351 init");
  ESP_RETURN_ON_ERROR(lab.rx_tdm ? init_i2s_tdm_rx() : init_i2s_std(), TAG,
                      "i2s init");
  ESP_RETURN_ON_ERROR(es7210_init_profile(lab), TAG, "es7210 init");
  ESP_RETURN_ON_ERROR(es8311_init_m5(lab.dac_volume), TAG, "es8311 init");
  ESP_RETURN_ON_ERROR(speaker_gate_set_enabled(true), TAG, "speaker gate enable");
  ESP_RETURN_ON_ERROR(amp_apply(lab.amp_sysctrl, lab.write_amp_gain,
                                lab.amp_gain),
                      TAG, "amp apply");
  vTaskDelay(pdMS_TO_TICKS(250));

  const uint8_t es7210_regs[] = {0x00, 0x01, 0x02, 0x03, 0x08, 0x11, 0x12,
                                 0x13, 0x40, 0x43, 0x45, 0x47, 0x49, 0x4b,
                                 0x4c};
  const uint8_t es8311_regs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                 0x07, 0x08, 0x09, 0x0a, 0x31, 0x32, 0x37};
  const uint8_t aw87559_regs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                  0x06, 0x07, 0x08, 0x09, 0x0a};
  dump_regs("ES7210", kAddrEs7210, es7210_regs,
            sizeof(es7210_regs) / sizeof(es7210_regs[0]));
  dump_regs("ES8311", kAddrEs8311, es8311_regs,
            sizeof(es8311_regs) / sizeof(es8311_regs[0]));
  dump_regs("AW87559", kAddrAw87559, aw87559_regs,
            sizeof(aw87559_regs) / sizeof(aw87559_regs[0]));

  ProbeMetrics metrics = {};
  esp_err_t result = run_probe(lab, &metrics);
  ESP_LOGI(TAG,
           "LAB_RESULT name=%s result=%s frames=%lu samples=%llu "
           "slot_peak=[%ld,%ld,%ld,%ld] slot_rms=[%.1f,%.1f,%.1f,%.1f] "
           "slot_corr=[%.3f,%.3f,%.3f,%.3f] aec_peak=%ld aec_rms=%.1f "
           "aec_ref_corr=%.3f suppression_db=%.2f",
           lab.name, esp_err_to_name(result),
           static_cast<unsigned long>(metrics.frames),
           static_cast<unsigned long long>(metrics.samples),
           static_cast<long>(metrics.slot_peak[0]),
           static_cast<long>(metrics.slot_peak[1]),
           static_cast<long>(metrics.slot_peak[2]),
           static_cast<long>(metrics.slot_peak[3]), metrics.slot_rms[0],
           metrics.slot_rms[1], metrics.slot_rms[2], metrics.slot_rms[3],
           metrics.slot_corr[0], metrics.slot_corr[1], metrics.slot_corr[2],
           metrics.slot_corr[3], static_cast<long>(metrics.aec_peak),
           metrics.aec_rms, metrics.aec_ref_corr, metrics.suppression_db);

  amp_apply(0x00, false, 0x00);
  speaker_gate_set_enabled(false);
  deinit_i2s();
  vTaskDelay(pdMS_TO_TICKS(500));
  return result;
}

}  // namespace

extern "C" void app_main(void) {
  ESP_LOGI(TAG, "Echo Pyramid AEC lab starting");
  esp_err_t err = init_i2c();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
    return;
  }

  const uint8_t probe_addrs[] = {kAddrEs8311, kAddrStm32,      kAddrEs7210,
                                 kAddrIoExpander, kAddrAw87559, kAddrSi5351};
  for (uint8_t addr : probe_addrs) {
    uint8_t reg00 = 0;
    err = read_reg(addr, 0x00, &reg00);
    ESP_LOGI(TAG, "I2C probe addr=0x%02x result=%s reg00=0x%02x", addr,
             esp_err_to_name(err), reg00);
  }

  for (const LabCase &lab : kCases) {
    err = run_case(lab);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Case %s failed: %s", lab.name, esp_err_to_name(err));
    }
  }

  ESP_LOGI(TAG, "Echo Pyramid AEC lab complete");
}
