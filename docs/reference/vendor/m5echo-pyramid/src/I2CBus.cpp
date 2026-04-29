/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "I2CBus.h"

// ================================================================
//                        Arduino
// ================================================================
#ifdef ARDUINO

bool I2CBus::begin(TwoWire* wire, uint8_t sda, uint8_t scl, uint32_t freq)
{
    _wire = wire;
    _wire->begin(sda, scl);
    _wire->setClock(freq);
    I2C_PRINTLN("I2C Init Done (Arduino/Wire)");
    return true;
}

bool I2CBus::writeReg(uint8_t addr, uint8_t reg, uint8_t value)
{
    _wire->beginTransmission(addr);
    _wire->write(reg);
    _wire->write(value);
    return (_wire->endTransmission() == 0);
}

bool I2CBus::writeBytes(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len)
{
    _wire->beginTransmission(addr);
    _wire->write(reg);
    _wire->write(data, len);
    uint8_t ret = _wire->endTransmission();
    return (ret == 0);
}

bool I2CBus::readBytes(uint8_t addr, uint8_t reg, uint8_t* data, size_t len)
{
    _wire->beginTransmission(addr);
    _wire->write(reg);
    _wire->endTransmission(false);

    _wire->requestFrom(addr, (uint8_t)len);
    for (size_t i = 0; i < len; i++) {
        data[i] = _wire->read();
    }
    return true;
}

// ================================================================
//                      ESP-IDF v5.x
// ================================================================
#else

static const char* TAG = "I2CBus";

bool I2CBus::begin(i2c_port_num_t port, int sda, int scl, uint32_t freq)
{
    _freq = freq;

    i2c_master_bus_config_t bus_cfg      = {};
    bus_cfg.i2c_port                     = port;
    bus_cfg.sda_io_num                   = (gpio_num_t)sda;
    bus_cfg.scl_io_num                   = (gpio_num_t)scl;
    bus_cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt            = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "I2C Init Done (IDF new driver) SDA=%d SCL=%d %luHz", sda, scl, (unsigned long)freq);
    return true;
}

i2c_master_dev_handle_t I2CBus::getDevHandle(uint8_t addr)
{
    for (uint8_t i = 0; i < _dev_count; i++) {
        if (_dev_cache[i].addr == addr) {
            return _dev_cache[i].handle;
        }
    }

    if (_dev_count >= I2C_DEV_CACHE_MAX) {
        ESP_LOGE(TAG, "Device cache full! (max %d)", I2C_DEV_CACHE_MAX);
        return nullptr;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length     = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address      = addr;
    dev_cfg.scl_speed_hz        = _freq;

    i2c_master_dev_handle_t handle = nullptr;
    esp_err_t err                  = i2c_master_bus_add_device(_bus_handle, &dev_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device(0x%02X) failed: %s", addr, esp_err_to_name(err));
        return nullptr;
    }

    _dev_cache[_dev_count].addr   = addr;
    _dev_cache[_dev_count].handle = handle;
    _dev_count++;

    ESP_LOGI(TAG, "Device 0x%02X added (total %d)", addr, _dev_count);
    return handle;
}

bool I2CBus::writeReg(uint8_t addr, uint8_t reg, uint8_t value)
{
    return writeBytes(addr, reg, &value, 1);
}

bool I2CBus::writeBytes(uint8_t addr, uint8_t reg, const uint8_t* data, size_t len)
{
    i2c_master_dev_handle_t dev = getDevHandle(addr);
    if (!dev) return false;

    uint8_t buf[64];
    if (len + 1 > sizeof(buf)) {
        ESP_LOGE(TAG, "writeBytes: len %u too large", (unsigned)len);
        return false;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);

    esp_err_t err = i2c_master_transmit(dev, buf, len + 1, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX 0x%02X reg 0x%02X fail: %s", addr, reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

bool I2CBus::readBytes(uint8_t addr, uint8_t reg, uint8_t* data, size_t len)
{
    i2c_master_dev_handle_t dev = getDevHandle(addr);
    if (!dev) return false;

    uint8_t write_buf[1] = {reg};

    esp_err_t err = i2c_master_transmit_receive(dev, write_buf, 1, data, len, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX 0x%02X reg 0x%02X fail: %s", addr, reg, esp_err_to_name(err));
        return false;
    }
    return true;
}

#endif  // ARDUINO vs ESP-IDF