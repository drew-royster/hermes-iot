/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "AW87559.h"

AW87559::AW87559(I2CBus &bus, uint8_t addr) : _bus(bus), _addr(addr)
{
}

bool AW87559::begin()
{
    uint8_t id;
    delay(10);
    if (!readReg(AW87559_REG_ID, id)) return false;

    if (id != 0x5A) {
        Serial.printf("AW87559 ID error: 0x%02X\n", id);
        return false;
    }
    Serial.println("AW87559 Ready");
    setSoftwareEnable(true);
    setBoostEnable(true);
    setPAEnable(true);
    setPAGain(AW87559_GAIN_16_5DB);
    return true;
}

/* ================= SYSCTRL ================= */

bool AW87559::setSoftwareEnable(bool en)
{
    return updateBits(AW87559_REG_SYSCTRL, AW87559_SYS_EN_SW_MASK, en ? AW87559_SYS_EN_SW_MASK : 0);
}

bool AW87559::setBoostEnable(bool en)
{
    return updateBits(AW87559_REG_SYSCTRL, AW87559_SYS_EN_BOOST_MASK, en ? AW87559_SYS_EN_BOOST_MASK : 0);
}

bool AW87559::setPAEnable(bool en)
{
    return updateBits(AW87559_REG_SYSCTRL, AW87559_SYS_EN_PA_MASK, en ? AW87559_SYS_EN_PA_MASK : 0);
}

bool AW87559::setReceiverMode(bool en)
{
    return updateBits(AW87559_REG_SYSCTRL, AW87559_SYS_RCV_MODE_MASK, en ? AW87559_SYS_RCV_MODE_MASK : 0);
}

bool AW87559::setHVBATEnable(bool en)
{
    return updateBits(AW87559_REG_SYSCTRL, AW87559_SYS_EN_HVBAT_MASK, en ? AW87559_SYS_EN_HVBAT_MASK : 0);
}

/* ================= BOOST ================= */

bool AW87559::setBoostVoltage(aw87559_bst_voltage_t voltage)
{
    if (voltage > AW87559_BST_9_5V) voltage = AW87559_BST_9_5V;
    return updateBits(AW87559_REG_BSTOVR, AW87559_BST_VOUT_MASK, (uint8_t)voltage);
}

bool AW87559::getBoostVoltage(aw87559_bst_voltage_t &voltage)
{
    uint8_t val;
    if (!readReg(AW87559_REG_BSTOVR, val)) return false;
    val &= AW87559_BST_VOUT_MASK;
    voltage = (aw87559_bst_voltage_t)val;

    return true;
}

/* ================= BAT ================= */

bool AW87559::enableBatteryProtection(bool en)
{
    return updateBits(AW87559_REG_BATSAFE, AW87559_BAT_EN_SFGD_MASK, en ? AW87559_BAT_EN_SFGD_MASK : 0);
}

bool AW87559::setBoostIPeak(aw87559_bst_ipeak_t ipeak)
{
    return updateBits(AW87559_REG_BSTCPR2, AW87559_BST_IPEAK_MASK, (uint8_t)ipeak);
}

bool AW87559::getBoostIPeak(aw87559_bst_ipeak_t &ipeak)
{
    uint8_t val;
    if (!readReg(AW87559_REG_BSTCPR2, val)) return false;
    val &= AW87559_BST_IPEAK_MASK;
    ipeak = (aw87559_bst_ipeak_t)val;
    return true;
}

/* ================= PA GAIN ================= */

bool AW87559::setPAGain(aw87559_gain_t gain)
{
    if (gain > AW87559_GAIN_27DB) gain = AW87559_GAIN_27DB;
    return updateBits(AW87559_REG_PAGR, AW87559_PAGR_GAIN_MASK, (uint8_t)gain);
}

float AW87559::getPAGain()
{
    uint8_t val;
    if (!readReg(AW87559_REG_PAGR, val)) return 0;
    val &= AW87559_PAGR_GAIN_MASK;
    return val * 1.5f;
}

/* ================= AGC3 ================= */

bool AW87559::enableAGC3(bool en)
{
    // 0 = enable, 1 = disable
    uint8_t val = en ? 0 : AW87559_AGC3_PD_MASK;
    return updateBits(AW87559_REG_AGC3PR, AW87559_AGC3_PD_MASK, val);
}

bool AW87559::setAGC3Power(aw87559_agc3_power_t power)
{
    if (power > AW87559_AGC3_2_0W) power = AW87559_AGC3_2_0W;
    return updateBits(AW87559_REG_AGC3PR, AW87559_AGC3_OUTPUT_POWER_MASK, (uint8_t)power);
}

bool AW87559::getAGC3Power(aw87559_agc3_power_t &power)
{
    uint8_t val;
    if (!readReg(AW87559_REG_AGC3PR, val)) return false;
    val &= AW87559_AGC3_OUTPUT_POWER_MASK;
    power = (aw87559_agc3_power_t)val;

    return true;
}

bool AW87559::setAGC3ReleaseTime(aw87559_agc3_release_t rel)
{
    return updateBits(AW87559_REG_AGC3_TIME, AW87559_AGC3_REL_MASK, ((uint8_t)rel << AW87559_AGC3_REL_SHIFT));
}

bool AW87559::setAGC3AttackTime(aw87559_agc3_attack_t att)
{
    return updateBits(AW87559_REG_AGC3_TIME, AW87559_AGC3_ATT_MASK, ((uint8_t)att << AW87559_AGC3_ATT_SHIFT));
}

bool AW87559::setAGC2Power(aw87559_agc2_power_t power)
{
    return updateBits(AW87559_REG_AGC2PR, AW87559_AGC2_OUTPUT_MASK, (uint8_t)power);
}

bool AW87559::setAGC2AttackTime(aw87559_agc2_attack_t att)
{
    return updateBits(AW87559_REG_AGC2_TIME, AW87559_AGC2_ATT_MASK, ((uint8_t)att << AW87559_AGC2_ATT_SHIFT));
}

bool AW87559::getAGC2AttackTime(aw87559_agc2_attack_t &att)
{
    uint8_t val;

    if (!readReg(AW87559_REG_AGC2_TIME, val)) return false;

    val = (val & AW87559_AGC2_ATT_MASK) >> AW87559_AGC2_ATT_SHIFT;

    att = (aw87559_agc2_attack_t)val;
    return true;
}

bool AW87559::setAGC1AttackTime(aw87559_agc1_attack_t att)
{
    return updateBits(AW87559_REG_AGC1PR, AW87559_AGC1_ATT_MASK, ((uint8_t)att << AW87559_AGC1_ATT_SHIFT));
}

bool AW87559::getAGC1AttackTime(aw87559_agc1_attack_t &att)
{
    uint8_t val;
    if (!readReg(AW87559_REG_AGC1PR, val)) return false;
    val = (val & AW87559_AGC1_ATT_MASK) >> AW87559_AGC1_ATT_SHIFT;
    att = (aw87559_agc1_attack_t)val;
    return true;
}

bool AW87559::enableAGC1(bool en)
{
    uint8_t val = en ? 0 : AW87559_AGC1_PD_MASK;

    return updateBits(AW87559_REG_AGC1PR, AW87559_AGC1_PD_MASK, val);
}

bool AW87559::setADPMode(aw87559_adp_mode_t mode)
{
    return updateBits(AW87559_REG_ADPMODE, AW87559_ADPBOOST_MODE_MASK, (uint8_t)mode);
}

bool AW87559::getADPMode(aw87559_adp_mode_t &mode)
{
    uint8_t val;
    if (!readReg(AW87559_REG_ADPMODE, val)) return false;
    val &= AW87559_ADPBOOST_MODE_MASK;
    mode = (aw87559_adp_mode_t)val;
    return true;
}

bool AW87559::setAGC1TimeAEnabled(bool en)
{
    uint8_t val = en ? AW87559_AGC1_ATT_TIMEA_MASK : 0;
    return updateBits(AW87559_REG_ADPMODE, AW87559_AGC1_ATT_TIMEA_MASK, val);
}

bool AW87559::getAGC1TimeAEnabled(bool &en)
{
    uint8_t val;
    if (!readReg(AW87559_REG_ADPMODE, val)) return false;
    en = (val & AW87559_AGC1_ATT_TIMEA_MASK) ? true : false;
    return true;
}

bool AW87559::setADPTime1(aw87559_adp_time_t time)
{
    return updateBits(AW87559_REG_ADPBST_TIME1, AW87559_ADP_BST_TIME1_MASK, (uint8_t)time);
}

bool AW87559::setADPTime2(aw87559_adp_time_t time)
{
    return updateBits(AW87559_REG_ADPBST_TIME1, AW87559_ADP_BST_TIME2_MASK,
                      ((uint8_t)time << AW87559_ADP_BST_TIME2_SHIFT));
}

bool AW87559::getADPTime1(aw87559_adp_time_t &time)
{
    uint8_t val;
    if (!readReg(AW87559_REG_ADPBST_TIME1, val)) return false;
    val &= AW87559_ADP_BST_TIME1_MASK;
    time = (aw87559_adp_time_t)val;
    return true;
}

bool AW87559::getADPTime2(aw87559_adp_time_t &time)
{
    uint8_t val;
    if (!readReg(AW87559_REG_ADPBST_TIME1, val)) return false;
    val  = (val & AW87559_ADP_BST_TIME2_MASK) >> AW87559_ADP_BST_TIME2_SHIFT;
    time = (aw87559_adp_time_t)val;
    return true;
}

bool AW87559::setADPLowStep(aw87559_adp_low_step_t step)
{
    return updateBits(AW87559_REG_ADPBST_VTH, AW87559_ADP_LOW_STEP_MASK, ((uint8_t)step << AW87559_ADP_LOW_STEP_SHIFT));
}

bool AW87559::getADPLowStep(aw87559_adp_low_step_t &step)
{
    uint8_t val;
    if (!readReg(AW87559_REG_ADPBST_VTH, val)) return false;

    step = (aw87559_adp_low_step_t)((val & AW87559_ADP_LOW_STEP_MASK) >> AW87559_ADP_LOW_STEP_SHIFT);

    return true;
}

bool AW87559::setBoostVTH1(aw87559_boost_vth1_t vth1)
{
    return updateBits(AW87559_REG_ADPBST_VTH, AW87559_SET_BOOST_VTH1_MASK,
                      ((uint8_t)vth1 << AW87559_SET_BOOST_VTH1_SHIFT));
}

bool AW87559::getBoostVTH1(aw87559_boost_vth1_t &vth1)
{
    uint8_t val;
    if (!readReg(AW87559_REG_ADPBST_VTH, val)) return false;

    vth1 = (aw87559_boost_vth1_t)((val & AW87559_SET_BOOST_VTH1_MASK) >> AW87559_SET_BOOST_VTH1_SHIFT);

    return true;
}

bool AW87559::setBoostVTH2(aw87559_boost_vth2_t vth2)
{
    return updateBits(AW87559_REG_ADPBST_VTH, AW87559_SET_BOOST_VTH2_MASK,
                      ((uint8_t)vth2 << AW87559_SET_BOOST_VTH2_SHIFT));
}

bool AW87559::getBoostVTH2(aw87559_boost_vth2_t &vth2)
{
    uint8_t val;
    if (!readReg(AW87559_REG_ADPBST_VTH, val)) return false;

    vth2 = (aw87559_boost_vth2_t)((val & AW87559_SET_BOOST_VTH2_MASK) >> AW87559_SET_BOOST_VTH2_SHIFT);

    return true;
}

bool AW87559::updateBits(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t val;
    if (!readReg(reg, val)) return false;
    val &= ~mask;
    val |= (value & mask);

    return writeReg(reg, val);
}

bool AW87559::writeReg(uint8_t reg, uint8_t val)
{
    return _bus.writeBytes(_addr, reg, &val, 1);
}

bool AW87559::readReg(uint8_t reg, uint8_t &val)
{
    return _bus.readBytes(_addr, reg, &val, 1);
}