/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "M5EchoPyramid.h"

M5EchoPyramid::M5EchoPyramid() : _si5351(_bus), _es7210(_bus), _es8311(_bus), _stm32(_bus), _pa(_bus)
{
}

// ================================================================
//  begin — Arduino
// ================================================================
#ifdef ARDUINO

bool M5EchoPyramid::begin(TwoWire* wire, uint8_t sda, uint8_t scl, int bclk, int lrck, int dout, int din,
                          uint32_t sample_rate)
{
    if (wire != nullptr) _wire = wire;

    _bus.begin(_wire, sda, scl);
    _si5351.begin();
    initI2S(bclk, lrck, dout, din, sample_rate);

    uint32_t mclk_hz = sample_rate * 256;
    _es7210.begin(mclk_hz, sample_rate, ES7210_INPUT_MIC1 | ES7210_INPUT_MIC3);
    _es8311.begin(mclk_hz, sample_rate);
    _stm32.begin();
    _pa.begin();
    return true;
}

#else
// ================================================================
//  begin — ESP-IDF
// ================================================================

bool M5EchoPyramid::begin(i2c_port_num_t i2c_port, int sda, int scl, int bclk, int lrck, int dout, int din,
                          uint32_t sample_rate)
{
    _bus.begin(i2c_port, sda, scl);
    _si5351.begin();
    initI2S(bclk, lrck, dout, din, sample_rate);

    uint32_t mclk_hz = sample_rate * 256;
    _es7210.begin(mclk_hz, sample_rate, ES7210_INPUT_MIC1 | ES7210_INPUT_MIC3);
    _es8311.begin(mclk_hz, sample_rate);
    _stm32.begin();
    _pa.begin();
    return true;
}

#endif

ES8311& M5EchoPyramid::codec()
{
    return _es8311;
}
ES7210& M5EchoPyramid::adc()
{
    return _es7210;
}
STM32Ctrl& M5EchoPyramid::ctrl()
{
    return _stm32;
}
AW87559& M5EchoPyramid::pa()
{
    return _pa;
}

void M5EchoPyramid::initI2S(int bclk, int lrck, int dout, int din, uint32_t sample_rate)
{
    uint32_t mclk = sample_rate * 256;

    switch (sample_rate) {
        case 16000:
        case 44100:
        case 48000:
            _si5351.setMCLK(mclk);
            break;
        default:
            Serial.printf("Unsupported sample rate: %u\n", (unsigned)sample_rate);
            return;
    }

    delay(10);

#if ESP_IDF_VERSION_MAJOR >= 5

    // ===============================
    // IDF 5.x  → NEW DRIVER
    // ===============================

    if (_tx_chan) {
        i2s_channel_disable(_tx_chan);
        i2s_del_channel(_tx_chan);
        _tx_chan = nullptr;
    }

    if (_rx_chan) {
        i2s_channel_disable(_rx_chan);
        i2s_del_channel(_rx_chan);
        _rx_chan = nullptr;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 256;

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &_tx_chan, &_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)bclk,
                .ws   = (gpio_num_t)lrck,
                .dout = (gpio_num_t)din,
                .din  = (gpio_num_t)dout,
            },
    };

    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(_rx_chan, &std_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(_tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(_rx_chan));

    Serial.printf("I2S Init OK (IDF5 New Driver) - %u Hz\n", (unsigned)sample_rate);

#else

    // ===============================
    // IDF 4.x → LEGACY DRIVER
    // ===============================

    i2s_config_t config;
    memset(&config, 0, sizeof(config));

    config.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);
    config.sample_rate          = sample_rate;
    config.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    config.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;
    config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    config.dma_buf_count        = 6;
    config.dma_buf_len          = 256;
    config.use_apll             = false;
    config.tx_desc_auto_clear   = true;
    config.fixed_mclk           = mclk;

    i2s_pin_config_t pin_config = {
        .bck_io_num   = bclk,
        .ws_io_num    = lrck,
        .data_out_num = din,
        .data_in_num  = dout,
    };

    i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.printf("I2S Init OK (IDF4 Legacy) - %u Hz\n", (unsigned)sample_rate);

#endif
}

void M5EchoPyramid::read(int16_t* mic, int16_t* ref, int frames)
{
    static int16_t buffer[256 * 2];
    size_t bytes      = frames * 2 * sizeof(int16_t);
    size_t read_bytes = 0;
#if ESP_IDF_VERSION_MAJOR >= 5

    i2s_channel_read(_rx_chan, buffer, bytes, &read_bytes, portMAX_DELAY);

#else

    i2s_read(I2S_NUM_0, buffer, bytes, &read_bytes, portMAX_DELAY);

#endif

    for (int i = 0; i < frames; i++) {
        mic[i] = buffer[i * 2 + 0];
        ref[i] = buffer[i * 2 + 1];
    }
}

void M5EchoPyramid::write(int16_t* data, int frames)
{
    static int16_t out[256 * 2];

    for (int i = 0; i < frames; i++) {
        out[i * 2 + 0] = data[i];
        out[i * 2 + 1] = data[i];
    }

    size_t bytes   = frames * 2 * sizeof(int16_t);
    size_t written = 0;

#if ESP_IDF_VERSION_MAJOR >= 5

    i2s_channel_write(_tx_chan, out, bytes, &written, portMAX_DELAY);

#else

    i2s_write(I2S_NUM_0, out, bytes, &written, portMAX_DELAY);

#endif
}