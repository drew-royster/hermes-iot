/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "SI5351.h"

#define XTAL_FREQ 27000000UL

SI5351::SI5351(I2CBus& bus, uint8_t addr) : _bus(bus), _addr(addr)
{
}

bool SI5351::writeReg(uint8_t reg, uint8_t val)
{
    return _bus.writeBytes(_addr, reg, &val, 1);
}

bool SI5351::writeBulk(uint8_t reg, uint8_t* data, uint8_t len)
{
    return _bus.writeBytes(_addr, reg, data, len);
}

bool SI5351::begin()
{
    // Turn off all outputs.
    writeReg(3, 0xFF);
    delay(10);

    // Power off all CLKS.
    writeReg(16, 0x80);  // CLK0 powerdown
    writeReg(17, 0x80);  // CLK1 powerdown
    writeReg(18, 0x80);  // CLK2 powerdown

    // Crystal oscillator load capacitance: 10pF
    writeReg(183, 0xC0);

    Serial.println("[SI5351] begin OK");
    return true;
}

void SI5351::resetPLL()
{
    writeReg(177, 0xA0);  // Reset PLLA
    delay(10);
}

bool SI5351::setPLL(uint32_t pll_freq, uint32_t ms_div)
{
    // Calculate PLLA parameters
    uint32_t a    = pll_freq / XTAL_FREQ;
    uint32_t rest = pll_freq % XTAL_FREQ;
    uint32_t c    = 1000000UL;
    uint32_t b    = (rest * c) / XTAL_FREQ;

    uint32_t P1 = 128 * a + (128 * b) / c - 512;
    uint32_t P2 = 128 * b - c * ((128 * b) / c);
    uint32_t P3 = c;

    // Turn off the output
    writeReg(3, 0xFF);

    // Write PLLA (Registers 26 to 33)
    uint8_t pll_buf[8];
    pll_buf[0] = (P3 >> 8) & 0xFF;
    pll_buf[1] = P3 & 0xFF;
    pll_buf[2] = (P1 >> 16) & 0x03;
    pll_buf[3] = (P1 >> 8) & 0xFF;
    pll_buf[4] = P1 & 0xFF;
    pll_buf[5] = ((P3 >> 12) & 0xF0) | ((P2 >> 16) & 0x0F);
    pll_buf[6] = (P2 >> 8) & 0xFF;
    pll_buf[7] = P2 & 0xFF;
    _bus.writeBytes(_addr, 26, pll_buf, 8);

    // Calculate the sub-sampling parameters of Multisynth1 (in integer mode)
    uint32_t MS_P1 = 128 * ms_div - 512;

    uint8_t ms_buf[8];
    ms_buf[0] = 0x00;
    ms_buf[1] = 0x01;  // P3 = 1
    ms_buf[2] = (MS_P1 >> 16) & 0x03;
    ms_buf[3] = (MS_P1 >> 8) & 0xFF;
    ms_buf[4] = MS_P1 & 0xFF;
    ms_buf[5] = 0x00;
    ms_buf[6] = 0x00;
    ms_buf[7] = 0x00;

    // ===== Only configure CLK1 [16] =====
    // CLK0 → NC is not configured.
    // CLK1 → R4 → R27(ES8311 MCLK_DAC) + R28(ES7210 MCLK_ADC)
    // CLK2 → Not used
    _bus.writeBytes(_addr, 50, ms_buf, 8);  // Multisynth1 → CLK1

    // CLK1 Control: Utilize PLLA, with 8mA drive
    writeReg(17, 0x4F);

    // CLK0/CLK2 Maintain power outage
    writeReg(16, 0x80);
    writeReg(18, 0x80);

    // PLL Reset
    writeReg(177, 0xA0);
    delay(10);

    // Only enable CLK1
    // bit0 = CLK0 off, bit1 = CLK1 on, bit2 = CLK2 off
    // 0xFD = 1111 1101
    writeReg(3, 0xFD);

    Serial.printf("[SI5351] CLK1 = %lu Hz (PLL=%lu, div=%lu)\n", pll_freq / ms_div, pll_freq, ms_div);
    return true;
}

bool SI5351::setupMultisynth(uint32_t div)
{
    uint32_t P1 = 128 * div - 512;
    uint32_t P2 = 0;
    uint32_t P3 = 1;

    uint8_t buf[8];

    buf[0] = (P3 >> 8) & 0xFF;
    buf[1] = P3 & 0xFF;
    buf[2] = (P1 >> 16) & 0x03;
    buf[3] = (P1 >> 8) & 0xFF;
    buf[4] = P1 & 0xFF;
    buf[5] = ((P3 >> 12) & 0xF0) | ((P2 >> 16) & 0x0F);
    buf[6] = (P2 >> 8) & 0xFF;
    buf[7] = P2 & 0xFF;

    if (!writeBulk(42, buf, 8)) return false;

    return true;
}

bool SI5351::setMCLK(uint32_t freq)
{
    if (freq == 11289600)  // 44100 × 256
        return setPLL(903168000UL, 80);

    if (freq == 12288000)  // 48000 × 256
        return setPLL(884736000UL, 72);

    if (freq == 4096000)  // 16000 × 256
        return setPLL(884736000UL, 216);

    Serial.printf("[SI5351] Unsupported MCLK: %lu Hz\n", freq);
    return false;
}

bool SI5351::setSampleRate(uint32_t sample_rate)
{
    switch (sample_rate) {
        case 16000:
            return setMCLK(4096000);

        case 44100:
            return setMCLK(11289600);

        case 48000:
            return setMCLK(12288000);

        default:
            return false;
    }
}