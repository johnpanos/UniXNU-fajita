// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita PMIC board helpers (FajitaPmicBoard.h): the charger init sequence, key decoding and debouncing, fuel-gauge conversions.
#include "FajitaPmicBoard.h"

// pmOS qcom_smbx smb2_init_seq (PMI8998), in order, then the probe's FAST_CHARGE_CURRENT_CFG and AICL_RERUN_TIME_CFG writes.
const fpm_write fpm_charge_seq_template[FPM_CHARGE_SEQ_LEN] = {
    {FPM_TYPE_C_INTRPT_ENB_SW_CTRL, 0x1f, 0x10, "typec UFP, VCONN_EN_SRC"},           // power role cmd 2:0, VCONN_EN_SRC 4, VCONN value 3
    {FPM_TYPE_C_CFG, 0x22, 0x00, "typec factory mode off, VCONN OC cfg"},             // FACTORY_MODE_DETECTION_EN 5, VCONN_OC_CFG 1
    {FPM_OTG_CFG, 0x02, 0x00, "OTG enable by software"},                              // OTG_EN_SRC_CFG 1
    {FPM_FG_UPDATE_CFG_2_SEL, 0x06, 0x02, "recharge threshold by VBAT"},              // SOC_LT_CHG_RECHARGE 2, VBT_LT_CHG_RECHARGE 1
    {FPM_USBIN_OPTIONS_1_CFG, 0x04, 0x00, "HVDCP off"},                               // HVDCP_EN 2
    {FPM_CHG_ENABLE_CMD, 0x01, 0x01, "charging enable"},
    {FPM_CHG_CFG2, 0xff, 0x01, "CHGR_CFG2: sw enable, inhibit"},                      // all eight bits, CHARGER_INHIBIT only
    {FPM_STAT_CFG, 0x40, 0x40, "STAT sw override"},
    {FPM_USBIN_ICL_OPTIONS, 0x03, 0x02, "USB51 mode"},                                // USB51_MODE 1, USBIN_MODE_CHG 0
    {FPM_SNARL_BARK_BITE_WD_CFG, 0xff, 0x00, "charger watchdog timeouts"},
    {FPM_WD_CFG, 0xc2, 0x00, "charger watchdog off"},                                 // AFP_EN 7, BARK_INT_EN 6, EN_ON_PLUGIN 1
    {FPM_USBIN_5V_AICL_THRESHOLD_CFG, 0x07, 0x03, "5V AICL threshold"},
    {FPM_USBIN_CONT_AICL_THRESHOLD_CFG, 0x3f, 0x03, "cont AICL threshold"},
    {FPM_USBIN_AICL_OPTIONS_CFG, 0xaf, 0x07, "AICL on, collapse responses"},          // START_AT_MAX 5, ADC_EN 3, EN 2, SUSP 7, HV 1, LV 0
    {FPM_PRE_CHARGE_CFG, 0x3f, 500000 / FPM_CURRENT_STEP_UA, "pre-charge 500 mA"},
    {FPM_FAST_CHARGE_CFG, 0xff, 0, "fast charge current"},
    {FPM_AICL_RERUN_TIME_CFG, 0x03, 0x00, "AICL rerun 3 s"},
};

static unsigned clampu(unsigned v, unsigned lo, unsigned hi) {return v < lo ? lo : v > hi ? hi : v;}

unsigned fpm_charge_sequence(fpm_write *out, unsigned capacity, unsigned fcc_ma, unsigned icl_ma) {
    if (!out || capacity < FPM_CHARGE_SEQ_LEN + 1) return 0;
    fcc_ma = clampu(fcc_ma ? fcc_ma : FPM_FCC_MA_DEFAULT, 100, FPM_FCC_MA_MAX);
    icl_ma = clampu(icl_ma ? icl_ma : FPM_ICL_MA_DEFAULT, 100, FPM_ICL_MA_MAX);
    for (unsigned i = 0; i < FPM_CHARGE_SEQ_LEN; ++i) {
        out[i] = fpm_charge_seq_template[i];
        if (out[i].addr == FPM_FAST_CHARGE_CFG) out[i].value = (uint8_t)(fcc_ma * 1000u / FPM_CURRENT_STEP_UA);
    }
    out[FPM_CHARGE_SEQ_LEN] = (fpm_write){FPM_USBIN_CURRENT_LIMIT_CFG, 0xff, (uint8_t)(icl_ma * 1000u / FPM_CURRENT_STEP_UA),
                                          "USB input current limit"};
    return FPM_CHARGE_SEQ_LEN + 1;
}

int fpm_sequence_is_safe(const fpm_write *seq, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        unsigned a = seq[i].addr, block = a >> 8;
        if (block < 0x10 || block > 0x16) return 0;
        if (a == FPM_FLOAT_VOLTAGE_CFG || (a >= 0x1090 && a <= 0x109f)) return 0;
        if (a == FPM_FAST_CHARGE_CFG && seq[i].value > FPM_FCC_MA_MAX * 1000u / FPM_CURRENT_STEP_UA) return 0;
        if (a == FPM_USBIN_CURRENT_LIMIT_CFG && seq[i].value > FPM_ICL_MA_MAX * 1000u / FPM_CURRENT_STEP_UA) return 0;
        if (a == FPM_PRE_CHARGE_CFG && (seq[i].value & seq[i].mask) > 500000 / FPM_CURRENT_STEP_UA) return 0;
    }
    return 1;
}

unsigned fpm_keys_decode(int pon_rt_sts, int vol_down_rt_sts, int vol_up_rt_sts) {
    unsigned keys = 0;
    if (pon_rt_sts >= 0 && (pon_rt_sts & FPM_PON_KPDPWR)) keys |= FPM_KEY_POWER;
    if (vol_up_rt_sts >= 0 && !(vol_up_rt_sts & 1)) keys |= FPM_KEY_VOL_UP;       // active low
    if (vol_down_rt_sts >= 0 && !(vol_down_rt_sts & 1)) keys |= FPM_KEY_VOL_DOWN;
    return keys;
}

unsigned fpm_debounce_step(fpm_debounce *d, unsigned sample, unsigned need) {
    if (sample == d->stable) {d->count = 0; d->candidate = sample; return d->stable;}
    if (sample != d->candidate) {d->candidate = sample; d->count = 1;}
    else ++d->count;
    if (d->count >= (need ? need : 1)) {d->stable = sample; d->count = 0;}
    return d->stable;
}

int fpm_fg_soc_percent(uint8_t b0, uint8_t b1) {
    unsigned cap = b0 < b1 ? b0 : b1;
    if (cap == 0) return 0;
    if (cap == 0xff) return 100;
    return (int)(((cap - 1) * 98 + 253 / 2) / 253 + 1);
}

const char *fpm_charger_state_name(uint8_t status_1) {
    static const char *const names[8] = {"trickle", "pre", "fast", "fullon", "taper", "terminate", "inhibit", "disabled"};
    return names[status_1 & 7];
}

int fpm_charging(uint8_t status_1, uint8_t status_2, int online) {
    if (!online || (status_2 & 0x20)) return 0;
    switch (status_1 & 7) {
    case FPM_CHG_TRICKLE: case FPM_CHG_PRE: case FPM_CHG_FAST: case FPM_CHG_FULLON: case FPM_CHG_TAPER: return 1;
    case FPM_CHG_TERMINATE: case FPM_CHG_INHIBIT: return 2;
    default: return 0;
    }
}
