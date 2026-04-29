/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ES8311.h"

// ===== Clock Coefficient Table =====
// Fully ported from reference driver coeff_div[] [17]
// mclk        rate   pre_div pre_multi adc_div dac_div fs  lrck_h lrck_l bclk adc_osr dac_osr
static const es8311_coeff_div coeff_div_table[] = {
    /* 8k */
    {12288000, 8000, 6, 0, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20},
    {18432000, 8000, 6, 0, 1, 1, 0, 0x00, 0xFF, 6, 0x10, 0x20},
    {16384000, 8000, 8, 0, 1, 1, 0, 0x00, 0xFF, 8, 0x10, 0x20},
    {8192000, 8000, 4, 0, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20},
    {6144000, 8000, 3, 0, 1, 1, 0, 0x00, 0xFF, 3, 0x10, 0x20},
    {4096000, 8000, 2, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {3072000, 8000, 1, 0, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20},
    {2048000, 8000, 1, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {1536000, 8000, 1, 0, 1, 1, 1, 0x00, 0x7F, 2, 0x10, 0x20},
    {1024000, 8000, 1, 0, 1, 1, 1, 0x00, 0x7F, 1, 0x10, 0x20},
    /* 11.025k */
    {11289600, 11025, 4, 0, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20},
    {5644800, 11025, 2, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {2822400, 11025, 1, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {1411200, 11025, 1, 0, 1, 1, 1, 0x00, 0x7F, 1, 0x10, 0x20},
    /* 12k */
    {12288000, 12000, 4, 0, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20},
    {6144000, 12000, 2, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {3072000, 12000, 1, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {1536000, 12000, 1, 0, 1, 1, 1, 0x00, 0x7F, 1, 0x10, 0x20},
    /* 16k */
    {12288000, 16000, 3, 0, 1, 1, 0, 0x00, 0xFF, 3, 0x10, 0x20},
    {18432000, 16000, 3, 0, 1, 1, 0, 0x00, 0xFF, 6, 0x10, 0x20},
    {16384000, 16000, 4, 0, 1, 1, 0, 0x00, 0xFF, 4, 0x10, 0x20},
    {8192000, 16000, 2, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {6144000, 16000, 1, 0, 1, 1, 0, 0x00, 0xFF, 3, 0x10, 0x20},
    {4096000, 16000, 1, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {3072000, 16000, 1, 0, 1, 1, 1, 0x00, 0x7F, 2, 0x10, 0x20},
    {2048000, 16000, 1, 0, 1, 1, 1, 0x00, 0x7F, 1, 0x10, 0x20},
    /* 22.05k */
    {11289600, 22050, 2, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {5644800, 22050, 1, 0, 1, 1, 0, 0x00, 0xFF, 1, 0x10, 0x20},
    {2822400, 22050, 1, 0, 1, 1, 1, 0x00, 0x7F, 1, 0x10, 0x20},
    /* 24k */
    {12288000, 24000, 2, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {6144000, 24000, 1, 0, 1, 1, 0, 0x00, 0xFF, 1, 0x10, 0x20},
    {3072000, 24000, 1, 0, 1, 1, 1, 0x00, 0x7F, 1, 0x10, 0x20},
    /* 32k */
    {12288000, 32000, 1, 0, 1, 1, 0, 0x00, 0xFF, 3, 0x10, 0x20},
    {18432000, 32000, 1, 0, 1, 1, 0, 0x00, 0xFF, 6, 0x10, 0x20},  // <- fixed
    {16384000, 32000, 1, 0, 2, 2, 0, 0x00, 0xFF, 4, 0x10, 0x20},
    {8192000, 32000, 1, 0, 1, 1, 0, 0x00, 0xFF, 2, 0x10, 0x20},
    {6144000, 32000, 1, 0, 1, 1, 1, 0x00, 0x7F, 3, 0x10, 0x20},
    {4096000, 32000, 1, 0, 1, 1, 1, 0x00, 0x7F, 2, 0x10, 0x20},
    {2048000, 32000, 1, 0, 1, 1, 1, 0x00, 0x7F, 1, 0x10, 0x20},
    /* 44.1k <- primary use case [1] */
    {11289600, 44100, 1, 0, 1, 1, 0, 0x00, 0xFF, 1, 0x10, 0x20},
    {5644800, 44100, 1, 0, 1, 1, 1, 0x00, 0x7F, 1, 0x10, 0x20},
    /* 48k */
    {12288000, 48000, 1, 0, 1, 1, 0, 0x00, 0xFF, 1, 0x10, 0x20},
    {18432000, 48000, 1, 0, 1, 1, 0, 0x00, 0xFF, 3, 0x10, 0x20},
    {16384000, 48000, 1, 0, 2, 2, 0, 0x00, 0xFF, 4, 0x10, 0x20},
    {8192000, 48000, 1, 0, 1, 1, 1, 0x00, 0x7F, 1, 0x10, 0x20},
    {6144000, 48000, 1, 0, 1, 1, 1, 0x00, 0x7F, 3, 0x10, 0x20},  // <- fixed
    {4096000, 48000, 1, 0, 1, 1, 1, 0x00, 0x7F, 2, 0x10, 0x20},
};

// ===== Constructor =====
ES8311::ES8311(I2CBus& bus, uint8_t addr) : _bus(bus), _addr(addr)
{
}

// ===== Low-level Read/Write (using I2CBus [8]) =====
bool ES8311::writeReg(uint8_t reg, uint8_t val)
{
    return _bus.writeBytes(_addr, reg, &val, 1);
}

bool ES8311::readReg(uint8_t reg, uint8_t& val)
{
    return _bus.readBytes(_addr, reg, &val, 1);
}

// ===== Look Up Coefficient Table =====
// Corresponds to the logic of traversing coeff_div[] in the reference driver [17]
const es8311_coeff_div* ES8311::getCoeff(uint32_t mclk, uint32_t rate)
{
    for (size_t i = 0; i < sizeof(coeff_div_table) / sizeof(coeff_div_table[0]); i++) {
        if (coeff_div_table[i].mclk == mclk && coeff_div_table[i].rate == rate) {
            return &coeff_div_table[i];
        }
    }
    return nullptr;
}

// ===== Clock Divider Configuration =====
// Fully ported from reference driver es8311_sample_frequency_config() [17]
bool ES8311::sampleFrequencyConfig(uint32_t mclk_hz, uint32_t sample_rate)
{
    // Use the official coefficient table directly [17]
    const es8311_coeff_div* coeff = getCoeff(mclk_hz, sample_rate);
    if (coeff == nullptr) {
        Serial.printf("[ES8311] Cannot find the coefficients: MCLK=%lu Rate=%lu\n", mclk_hz, sample_rate);
        return false;
    }

    uint8_t regv = 0;

    // REG02: pre_div & pre_multi
    readReg(ES8311_CLK_MANAGER_REG02, regv);
    regv &= 0x07;
    regv |= (uint8_t)((coeff->pre_div - 1) << 5);
    regv |= (uint8_t)(coeff->pre_multi << 3);
    writeReg(ES8311_CLK_MANAGER_REG02, regv);

    // REG03: fs_mode & adc_osr
    writeReg(ES8311_CLK_MANAGER_REG03, (uint8_t)((coeff->fs_mode << 6) | coeff->adc_osr));

    // REG04: dac_osr
    writeReg(ES8311_CLK_MANAGER_REG04, coeff->dac_osr);

    // REG05: adc_div & dac_div
    writeReg(ES8311_CLK_MANAGER_REG05, (uint8_t)(((coeff->adc_div - 1) << 4) | (coeff->dac_div - 1)));

    // REG06: bclk_div (preserve polarity bit)
    readReg(ES8311_CLK_MANAGER_REG06, regv);
    regv &= 0xE0;
    if (coeff->bclk_div < 19)
        regv |= (uint8_t)(coeff->bclk_div - 1);
    else
        regv |= (uint8_t)(coeff->bclk_div);
    writeReg(ES8311_CLK_MANAGER_REG06, regv);

    // REG07: lrck_h (preserve upper 2 bits)
    readReg(ES8311_CLK_MANAGER_REG07, regv);
    regv &= 0xC0;
    regv |= coeff->lrck_h;
    writeReg(ES8311_CLK_MANAGER_REG07, regv);

    // REG08: lrck_l
    writeReg(ES8311_CLK_MANAGER_REG08, coeff->lrck_l);

    Serial.printf("[ES8311] Frequency division configuration is OK: MCLK=%lu Rate=%lu\n", mclk_hz, sample_rate);
    return true;
}

// ===== Clock Source Configuration =====
// Corresponds to reference driver es8311_clock_config() [17]
// Per schematic [16], ES8311 MCLK comes from SI5351 CLK1 (I2S_MCLK_DAC) via the MCLK pin
bool ES8311::clockConfig(uint32_t mclk_hz)
{
    // REG01: Enable all clocks, use MCLK pin [17]
    // bit7=0: Use MCLK pin (not BCLK)
    // 0x3F = 0b00111111: All clocks enabled
    uint8_t reg01 = 0x3F;
    writeReg(ES8311_CLK_MANAGER_REG01, reg01);

    // REG06: BCLK not inverted [17]
    uint8_t reg06 = 0;
    readReg(ES8311_CLK_MANAGER_REG06, reg06);
    reg06 &= ~(1 << 5);  // sclk_inverted = false
    writeReg(ES8311_CLK_MANAGER_REG06, reg06);

    return true;
}

// ===== Main Initialization Function =====
// Ported from reference driver es8311_init() [17]
bool ES8311::begin(uint32_t mclk_hz, uint32_t sample_rate)
{
    // Reset [17]
    writeReg(ES8311_RESET_REG00, 0x1F);
    delay(20);
    writeReg(ES8311_RESET_REG00, 0x00);
    writeReg(ES8311_RESET_REG00, 0x80);

    // Clock source: Use MCLK pin, fed by SI5351 CLK1 [16][17]
    // REG01 = 0x3F: Enable all clocks, bit7=0 means use MCLK pin
    writeReg(ES8311_CLK_MANAGER_REG01, 0x3F);

    // Look up table for divider configuration [17]
    if (!sampleFrequencyConfig(mclk_hz, sample_rate)) {
        return false;
    }

    // Slave mode, 16-bit I2S [17]
    uint8_t reg00 = 0;
    readReg(ES8311_RESET_REG00, reg00);
    reg00 &= ~(1 << 6);  // bit6=0: Slave
    writeReg(ES8311_RESET_REG00, reg00);
    writeReg(ES8311_SDPIN_REG09, 0x0C);   // 16-bit I2S
    writeReg(ES8311_SDPOUT_REG0A, 0x0C);  // 16-bit I2S

    // Analog power configuration [17]
    writeReg(ES8311_SYSTEM_REG0D, 0x01);
    writeReg(ES8311_SYSTEM_REG0E, 0x02);
    writeReg(ES8311_SYSTEM_REG12, 0x00);
    writeReg(ES8311_SYSTEM_REG13, 0x10);

    writeReg(ES8311_ADC_REG1C, 0x6A);
    writeReg(ES8311_DAC_REG37, 0x08);

    setVolume(40);

    Serial.println("[ES8311] Init OK");
    return true;
}

// ===== Volume Control [6] =====
bool ES8311::setVolume(uint8_t volume)
{
    if (volume > 100) volume = 100;

    uint8_t reg;
    if (volume == 0)
        reg = 0;
    else
        reg = (uint8_t)(((volume * 256) / 100) - 1);

    writeReg(ES8311_DAC_REG34, 0x80);
    writeReg(ES8311_DAC_REG35, 0x90);
    return writeReg(ES8311_DAC_REG32, reg);
}

bool ES8311::getVolume(uint8_t& volume)
{
    uint8_t reg;
    if (!readReg(ES8311_DAC_REG32, reg)) return false;

    if (reg == 0)
        volume = 0;
    else
        volume = (uint8_t)(((reg * 100) / 256) + 1);

    return true;
}

// ===== Mute [6] =====
bool ES8311::mute(bool enable)
{
    uint8_t reg;
    if (!readReg(ES8311_DAC_REG31, reg)) return false;

    if (enable)
        reg |= (1 << 6) | (1 << 5);
    else
        reg &= ~((1 << 6) | (1 << 5));

    return writeReg(ES8311_DAC_REG31, reg);
}

// ===== MIC Gain =====
bool ES8311::setMicGain(es8311_mic_gain_t gain)
{
    uint8_t reg_val = static_cast<uint8_t>(gain);
    return writeReg(ES8311_ADC_REG16, reg_val);
}