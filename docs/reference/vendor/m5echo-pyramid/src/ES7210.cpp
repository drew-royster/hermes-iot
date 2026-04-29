/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "ES7210.h"

ES7210::ES7210(I2CBus& bus, uint8_t addr) : _bus(bus), _addr(addr)
{
}

bool ES7210::writeReg(uint8_t reg, uint8_t val)
{
    bool ok = _bus.writeBytes(_addr, reg, &val, 1);
    if (!ok) {
        Serial.printf("[ES7210] Write FAILED: REG[0x%02X] = 0x%02X\n", reg, val);
    }
    return ok;
}

bool ES7210::readReg(uint8_t reg, uint8_t& val)
{
    return _bus.readBytes(_addr, reg, &val, 1);
}

bool ES7210::begin(uint32_t mclk_hz, uint32_t sample_rate, uint8_t mic_mask)
{
    Serial.println("=== ES7210 Init Start ===");

    uint8_t chip_id = 0;
    readReg(0x3D, chip_id);
    Serial.printf("[ES7210] Chip ID reg(0x3D) = 0x%02X\n", chip_id);

    // ──────────────────────────────────────────────
    // 1. Software reset
    // ──────────────────────────────────────────────
    writeReg(ES7210_RESET_REG00, 0xFF);
    delay(20);
    writeReg(ES7210_RESET_REG00, 0x32);
    delay(20);

    // ──────────────────────────────────────────────
    // 2. Clock configuration
    // ──────────────────────────────────────────────

    // turn off all the clocks to prevent any problems during the configuration process.
    writeReg(ES7210_CLOCK_OFF_REG01, 0x3F);

    // The MCLK signal is derived from an external pin (SI5351 CLK1 → R28 → ES7210 MCLK )
    // REG03: bit[7:6] = 00; MCLK is input from pin, bit[3:0] = SCLK divided by a factor
    writeReg(ES7210_MASTER_CLK_REG03, 0x04);

    // Calculate LRCK frequency division: LRCK_DIV = MCLK / sample_rate
    uint16_t lrck_div = (uint16_t)(mclk_hz / sample_rate);  // 256 for 44100
    writeReg(ES7210_LRCK_DIVH_REG04, (uint8_t)((lrck_div >> 8) & 0xFF));
    writeReg(ES7210_LRCK_DIVL_REG05, (uint8_t)(lrck_div & 0xFF));

    // Main clock division
    // For 256Fs, no additional division is required internally
    writeReg(ES7210_MAINCLK_REG02, 0x01);

    // OSR Configuration
    writeReg(ES7210_OSR_REG07, 0x20);

    Serial.printf("[ES7210] MCLK=%lu Hz  FS=%lu Hz  LRCK_DIV=%u\n", mclk_hz, sample_rate, lrck_div);

    // ──────────────────────────────────────────────
    // 3. Slave mode + I2S format
    // ──────────────────────────────────────────────

    // REG08: Slave mode, without using TDM
    // bit[7:6]=00 slave, bit[5]=0 normal, bit[4]=0 non-TDM
    writeReg(ES7210_MODE_CONFIG_REG08, 0x20);

    // REG11: I2S standard format, 16 bits
    // bit[7:6]=01 I2S format, bit[5:4]=01 16bit, bit[3:2]=00
    writeReg(ES7210_SDP_INTERFACE1_REG11, 0x60);

    // REG12: Normal pin state
    writeReg(ES7210_SDP_INTERFACE2_REG12, 0x00);

    // ──────────────────────────────────────────────
    // 4. Power-on of the simulation section
    // ──────────────────────────────────────────────

    // REG40: Simulation of Power Supply Enable
    // Bit[6] = 1 enables the simulation of reference voltage, Bit[1] = 1 enables the simulation of power supply
    writeReg(ES7210_ANALOG_REG40, 0x42);

    // REG41: MIC1/2 Bias Voltage Enable
    // Bits [6:4] = 111 represent the bias voltage level, Bit [3] = 0, and Bits [2:0] = bias enable.
    writeReg(ES7210_MIC12_BIAS_REG41, 0x70);

    // REG42: MIC3/4 Bias voltage enable
    writeReg(ES7210_MIC34_BIAS_REG42, 0x70);

    Serial.println("[ES7210] Analog power ON");

    // ──────────────────────────────────────────────
    // 5. Microphone gain setting
    // ──────────────────────────────────────────────

    // 默认设 24dB 增益（0x0C），足够拾音
    writeReg(ES7210_MIC1_GAIN_REG43, 0x1C);
    writeReg(ES7210_MIC2_GAIN_REG44, 0x1C);
    writeReg(ES7210_MIC3_GAIN_REG45, 0x1C);
    writeReg(ES7210_MIC4_GAIN_REG46, 0x1C);

    Serial.println("[ES7210] MIC gain set to ~24dB");

    // ──────────────────────────────────────────────
    // 6. Power on the microphone channel
    // ──────────────────────────────────────────────

    // REG47~4A: Each MIC's PGA is powered on separately.
    writeReg(ES7210_MIC1_POWER_REG47, 0x08);
    writeReg(ES7210_MIC2_POWER_REG48, 0x08);
    writeReg(ES7210_MIC3_POWER_REG49, 0x08);
    writeReg(ES7210_MIC4_POWER_REG4A, 0x08);

    // REG4B: ADC + PGA of MIC1/2's total switch
    // bit[3]=MIC2_ADC_ON, bit[2]=MIC2_PGA_ON, bit[1]=MIC1_ADC_ON, bit[0]=MIC1_PGA_ON
    // 0x0F = Fully open
    writeReg(ES7210_MIC12_POWER_REG4B, 0x0F);

    // REG4C: ADC + PGA of MIC3/4 combined switch
    writeReg(ES7210_MIC34_POWER_REG4C, 0x0F);

    Serial.println("[ES7210] MIC channel power ON");

    // ──────────────────────────────────────────────
    // 7. Turn off the mute function
    // ──────────────────────────────────────────────
    writeReg(ES7210_ADC_AUTOMUTE_REG13, 0x00);

    // ──────────────────────────────────────────────
    // 8. Power management - Ensure that it does not enter the power-down state
    // ──────────────────────────────────────────────
    writeReg(ES7210_POWER_DOWN_REG06, 0x00);

    // ──────────────────────────────────────────────
    // 9. Turn on the clock
    // ──────────────────────────────────────────────
    writeReg(ES7210_CLOCK_OFF_REG01, 0x00);

    // ──────────────────────────────────────────────
    // 10. Start the state machine
    // ──────────────────────────────────────────────
    writeReg(ES7210_RESET_REG00, 0x71);
    delay(100);
    writeReg(ES7210_RESET_REG00, 0x41);
    delay(50);

    Serial.println("[ES7210] Init Complete");
    return true;
}

bool ES7210::setMicGain(uint8_t mic, es7210_gain_value_t gain)
{
    uint8_t reg;

    switch (mic) {
        case 1:
            reg = ES7210_MIC1_GAIN_REG43;
            break;
        case 2:
            reg = ES7210_MIC2_GAIN_REG44;
            break;
        case 3:
            reg = ES7210_MIC3_GAIN_REG45;
            break;
        case 4:
            reg = ES7210_MIC4_GAIN_REG46;
            break;
        default:
            return false;
    }
    writeReg(reg, 0x10);
    return writeReg(reg, 0x10 | (gain & 0x0F));
}

bool ES7210::mute(bool enable)
{
    return writeReg(ES7210_ADC_AUTOMUTE_REG13, enable ? 0xFF : 0x00);
}

bool ES7210::powerMic(uint8_t mic, bool enable)
{
    uint8_t reg;
    switch (mic) {
        case 1:
            reg = ES7210_MIC1_POWER_REG47;
            break;
        case 2:
            reg = ES7210_MIC2_POWER_REG48;
            break;
        case 3:
            reg = ES7210_MIC3_POWER_REG49;
            break;
        case 4:
            reg = ES7210_MIC4_POWER_REG4A;
            break;
        default:
            return false;
    }
    return writeReg(reg, enable ? 0x08 : 0x00);
}