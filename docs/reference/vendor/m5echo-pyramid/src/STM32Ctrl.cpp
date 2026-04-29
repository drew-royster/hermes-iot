/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "STM32Ctrl.h"

STM32Ctrl::STM32Ctrl(I2CBus& bus, uint8_t addr) : _bus(bus), _addr(addr)
{
}

bool STM32Ctrl::begin()
{
    resetSpeaker();
    delay(100);
    return true;
}

/* ===================== Touch ===================== */

uint8_t STM32Ctrl::getTouchRaw()
{
    uint8_t buf[4] = {0};

    // Read 4 bytes continuously from 0x0
    if (!_bus.readBytes(_addr, TOUCH_BUTTON_STATUS_REG_ADDR, buf, 4)) return 0;

    uint8_t state = 0;

    if (buf[0]) state |= 0x01;  // Touch1
    if (buf[1]) state |= 0x02;  // Touch2
    if (buf[2]) state |= 0x04;  // Touch3
    if (buf[3]) state |= 0x08;  // Touch4

    return state;
}

bool STM32Ctrl::isPressed(uint8_t index)
{
    if (index < 1 || index > 4) return false;
    return (getTouchRaw() >> (index - 1)) & 0x01;
}

/* ===================== Brightness ===================== */

bool STM32Ctrl::setBrightness(uint8_t channel, uint8_t value)
{
    if (value > 100) value = 100;

    uint8_t reg = RGB1_BRIGHTNESS_REG_ADDR + (channel - 1);
    return _bus.writeReg(_addr, reg, value);
}

uint8_t STM32Ctrl::getBrightness(uint8_t channel)
{
    if (channel < 1 || channel > 2) return 0;

    uint8_t reg = RGB1_BRIGHTNESS_REG_ADDR + (channel - 1);

    uint8_t value = 0;
    _bus.readBytes(_addr, reg, &value, 1);

    return value;
}

/* ===================== RGB ===================== */

uint8_t STM32Ctrl::calcRGBBaseReg(uint8_t channel, uint8_t ledIndex)
{
    // One RGB every 3 bytes (B G R)
    uint8_t base = (channel == 1) ? RGB1_STATUS_REG_ADDR : RGB2_STATUS_REG_ADDR;

    // 16 bytes per page, a total of 4 RGB, each 4 bytes.
    return base + ledIndex * 4;
}

bool STM32Ctrl::setRGB(uint8_t channel, uint8_t ledIndex, uint8_t r, uint8_t g, uint8_t b)
{
    if (ledIndex > RGB_NUM_MAX) ledIndex = RGB_NUM_MAX;

    uint8_t reg = calcRGBBaseReg(channel, ledIndex);

    uint8_t data[4] = {b, g, r, 0x00};
    return _bus.writeBytes(_addr, reg, data, 4);
}

bool STM32Ctrl::writeRawPage(uint8_t channel, uint8_t page, uint8_t* data)
{
    uint8_t base = (channel == 1) ? RGB1_STATUS_REG_ADDR : RGB2_STATUS_REG_ADDR;

    uint8_t reg = base + page * 0x10;

    return _bus.writeBytes(_addr, reg, data, 12);
}

bool STM32Ctrl::setAllRGB(uint8_t channel, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t data[12];

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            int idx       = j * 3;
            data[idx + 0] = b;
            data[idx + 1] = g;
            data[idx + 2] = r;
        }

        writeRawPage(channel, i, data);
    }

    return true;
}

bool STM32Ctrl::getRGB(uint8_t channel, uint8_t ledIndex, uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (ledIndex > RGB_NUM_MAX) ledIndex = RGB_NUM_MAX;

    uint8_t base = (channel == 1) ? RGB1_STATUS_REG_ADDR : RGB2_STATUS_REG_ADDR;

    uint8_t buffer[56];

    if (!_bus.readBytes(_addr, base, buffer, 56)) return false;

    uint8_t* led = &buffer[ledIndex * 4];
    b            = led[0];
    g            = led[1];
    r            = led[2];
    return true;
}

/* ===================== System ===================== */

bool STM32Ctrl::resetSpeaker()
{
    return _bus.writeReg(_addr, SPK_RESTART_REG_ADDR, 1);
}

bool STM32Ctrl::getUSB5V(uint16_t& mv)
{
    uint8_t buf[2] = {0};

    if (!_bus.readBytes(_addr, READ_USB5V_REG_ADDR, buf, 2)) {
        mv = 0;
        return false;
    }

    mv = (uint16_t)((buf[1] << 8) | buf[0]);

    return true;
}

bool STM32Ctrl::saveBrightness(uint8_t channel)
{
    if (channel == 1)
        return _bus.writeReg(_addr, FLASH_WRITE_BACK_REG_ADDR, 1);
    else if (channel == 2)
        return _bus.writeReg(_addr, FLASH_WRITE_BACK_REG_ADDR, 2);

    return false;
}

uint8_t STM32Ctrl::getFirmwareVersion()
{
    uint8_t ver = 0;
    _bus.readBytes(_addr, SW_VER_REG_ADDR, &ver, 1);
    return ver;
}

bool STM32Ctrl::setI2CAddress(uint8_t newAddr)
{
    if (newAddr < 0x08 || newAddr > 0x77) return false;

    bool ok = _bus.writeReg(_addr, I2C_ADDR_REG_ADDR, newAddr);

    if (ok) _addr = newAddr;
    return ok;
}

uint8_t STM32Ctrl::getI2CAddress()
{
    uint8_t addr = 0;
    _bus.readBytes(_addr, I2C_ADDR_REG_ADDR, &addr, 1);
    return addr;
}