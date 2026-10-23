// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita (OnePlus 6T) PMIC board constants and pure helpers, host-tested by tests/fajita-pmic-board-test.cpp.
//
// Sources (Linux v6.16 and the postmarketOS sdm845 kernel 6.16, studied, no code copied):
// - pm8998.dtsi / pmi8998.dtsi: PM8998 on SID 0 (PON 0x800, GPIOs 0xc000), PMI8998 on SID 2 (charger 0x1000, RRADC 0x4500,
//   fuel gauge 0x4000).
// - sdm845-oneplus-common.dtsi: gpio-keys "Volume down" = PM8998 GPIO 5, "Volume up" = PM8998 GPIO 6, both active low with a pull-up;
//   the power key is the PON KPDPWR line (pm8941-pwrkey: PON_RT_STS 0x10 bit 0). RESIN is disabled on this board.
// - drivers/pinctrl/qcom/pinctrl-spmi-gpio.c: GPIO n at 0xc000 + 0x100 * (n - 1), RT_STS 0x10 bit 0 is the input level.
// - drivers/power/supply/qcom_smbx.c (pmOS, the driver charge.sh loads): smb2_init_seq, FAST_CHARGE_CURRENT_CFG 1.95 A, AICL rerun
//   3 s, USBIN_CURRENT_LIMIT_CFG = mA / 25. The float voltage write of its probe is NOT done here (left as the bootloader set it).
// - drivers/power/supply/qcom_fg.c (pmOS, gen3 ops): MSOC 0x4009 (two bytes), BATT_INFO voltage 0x41a0 (122.07 uV/LSB), current
//   0x41a2 (s16, 488.281 uA/LSB, negative while charging), temperature 0x4150 (0.25 K/LSB, 11 bits).
#ifndef FAJITA_PMIC_BOARD_H
#define FAJITA_PMIC_BOARD_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {FPM_SID_PM8998 = 0, FPM_SID_PMI8998 = 2};

// REVID (0x100 on every PMIC): TYPE 0x104 = 0x51 (PMIC), SUBTYPE 0x105.
#define FPM_REVID_TYPE 0x104
#define FPM_REVID_SUBTYPE 0x105
#define FPM_REVID_REV4 0x103
#define FPM_PMIC_TYPE 0x51
#define FPM_SUBTYPE_PM8998 0x14
#define FPM_SUBTYPE_PMI8998 0x15

// Keys.
#define FPM_PON_RT_STS 0x0810
#define FPM_PON_KPDPWR 0x01u
#define FPM_GPIO_BASE(n) (0xc000u + 0x100u * ((n) - 1u))
#define FPM_GPIO_RT_STS 0x10
#define FPM_GPIO_MODE_CTL 0x40
#define FPM_GPIO_DIG_PULL 0x42
#define FPM_GPIO_EN_CTL 0x46
#define FPM_GPIO_VOL_DOWN 5u
#define FPM_GPIO_VOL_UP 6u
enum {FPM_KEY_POWER = 1u << 0, FPM_KEY_VOL_UP = 1u << 1, FPM_KEY_VOL_DOWN = 1u << 2};

// Charger (PMI8998 SMB2, base 0x1000).
#define FPM_CHG_STATUS_1 0x1006       // bits 2:0 charger state
#define FPM_CHG_STATUS_2 0x1007       // bit 5 battery over-voltage, 3:0 temperature status
#define FPM_CHG_STATUS_7 0x100d
#define FPM_CHG_ENABLE_CMD 0x1042
#define FPM_CHG_CFG2 0x1051
#define FPM_PRE_CHARGE_CFG 0x1060
#define FPM_FAST_CHARGE_CFG 0x1061
#define FPM_FLOAT_VOLTAGE_CFG 0x1070
#define FPM_FG_UPDATE_CFG_2_SEL 0x107d
#define FPM_JEITA_EN_CFG 0x1090
#define FPM_OTG_CFG 0x1153
#define FPM_APSD_STATUS 0x1307
#define FPM_APSD_RESULT 0x1308
#define FPM_USB_INT_RT_STS 0x1310     // bit 4 USBIN plugged in
#define FPM_TYPE_C_STATUS_4 0x130e
#define FPM_USBIN_CMD_IL 0x1340       // bit 0 USBIN suspend
#define FPM_TYPE_C_CFG 0x1358
#define FPM_USBIN_OPTIONS_1_CFG 0x1362
#define FPM_USBIN_ICL_OPTIONS 0x1366
#define FPM_TYPE_C_INTRPT_ENB_SW_CTRL 0x1368
#define FPM_USBIN_CURRENT_LIMIT_CFG 0x1370
#define FPM_USBIN_AICL_OPTIONS_CFG 0x1380
#define FPM_USBIN_5V_AICL_THRESHOLD_CFG 0x1381
#define FPM_USBIN_CONT_AICL_THRESHOLD_CFG 0x1384
#define FPM_ICL_STATUS 0x1607
#define FPM_POWER_PATH_STATUS 0x160b
#define FPM_WD_CFG 0x1651
#define FPM_SNARL_BARK_BITE_WD_CFG 0x1653
#define FPM_AICL_RERUN_TIME_CFG 0x1661
#define FPM_STAT_CFG 0x1690
#define FPM_CURRENT_STEP_UA 25000u     // charger current registers: 1/40 A per LSB

// Fuel gauge (PMI8998 FG gen3, base 0x4000).
#define FPM_FG_MSOC 0x4009
#define FPM_FG_BATT_TEMP 0x4150
#define FPM_FG_BATT_VOLTAGE 0x41a0
#define FPM_FG_BATT_CURRENT 0x41a2

// Limits of this board: the pmOS values (charge.sh raises the input limit to 1.5 A; qcom_smbx sets 1.95 A fast charge).
#define FPM_ICL_MA_DEFAULT 1500u
#define FPM_ICL_MA_MAX 1500u
#define FPM_FCC_MA_DEFAULT 1000u       // the mainline driver's value; fajita-pmic-fcc=<mA> up to the pmOS 1950
#define FPM_FCC_MA_MAX 1950u

typedef struct fpm_write {
    uint16_t addr;
    uint8_t mask;
    uint8_t value;
    const char *what;
} fpm_write;

// The pmOS qcom_smbx PMI8998 init (smb2_init_seq, then the probe's fast-charge current and AICL rerun time), in its order, on SID 2.
// FAST_CHARGE_CURRENT_CFG's value is filled in by fpm_charge_sequence from the requested current.
#define FPM_CHARGE_SEQ_LEN 17u
extern const fpm_write fpm_charge_seq_template[FPM_CHARGE_SEQ_LEN];
// Copies the template with the fast-charge and input currents (both clamped to the board limits); the input limit is last.
// Returns the number of entries written to out (FPM_CHARGE_SEQ_LEN + 1).
unsigned fpm_charge_sequence(fpm_write *out, unsigned capacity, unsigned fcc_ma, unsigned icl_ma);
// 1 when every entry stays clear of the protection registers (float voltage, JEITA, PON) and inside the charger's 0x10..0x16 blocks.
int fpm_sequence_is_safe(const fpm_write *seq, unsigned count);
static inline uint8_t fpm_apply(uint8_t old, uint8_t mask, uint8_t value) {return (uint8_t)((old & ~mask) | (value & mask));}

// Key mask from PON_RT_STS and the two GPIO RT_STS reads (a negative value is a failed read: that key reads released).
unsigned fpm_keys_decode(int pon_rt_sts, int vol_down_rt_sts, int vol_up_rt_sts);
// Debounce: a key changes state after `need` consecutive identical samples. Returns the debounced mask.
typedef struct fpm_debounce {
    unsigned stable, candidate, count;
} fpm_debounce;
unsigned fpm_debounce_step(fpm_debounce *d, unsigned sample, unsigned need);

// Conversions.
static inline int32_t fpm_fg_voltage_mv(uint8_t lsb, uint8_t msb) {return (int32_t)(((uint32_t)(msb << 8 | lsb) * 122070u + 500000u) / 1000000u);}
// Positive while the battery takes charge (the PMIC's sign inverted, as power_supply expects).
static inline int32_t fpm_fg_current_ma(uint8_t lsb, uint8_t msb) {
    int32_t raw = (int16_t)(uint16_t)(msb << 8 | lsb);
    int64_t ua = (int64_t)raw * 488281 / 1000;
    return (int32_t)(ua >= 0 ? -((ua + 500) / 1000) : (-ua + 500) / 1000);
}
// Tenths of a degree Celsius.
static inline int32_t fpm_fg_temp_dc(uint8_t lsb, uint8_t msb) {
    int32_t raw = ((msb & 7) << 8) | lsb;
    return (raw * 10 + 2) / 4 - 2730;
}
// Monotonic SoC (0..255 across both bytes; the smaller of the pair) to percent, as qcom_fg_get_capacity (which tops out at 99:
// 0xff, the gauge's full, is 100 here).
int fpm_fg_soc_percent(uint8_t b0, uint8_t b1);

enum {FPM_CHG_TRICKLE, FPM_CHG_PRE, FPM_CHG_FAST, FPM_CHG_FULLON, FPM_CHG_TAPER, FPM_CHG_TERMINATE, FPM_CHG_INHIBIT, FPM_CHG_DISABLE};
const char *fpm_charger_state_name(uint8_t status_1);
// 1 charging, 0 not charging, 2 full (terminate/inhibit), from STATUS_1/STATUS_2 with the input online.
int fpm_charging(uint8_t status_1, uint8_t status_2, int online);
static inline int fpm_usb_online(uint8_t power_path) {return (power_path & 0x10) && (power_path & 0x01);}
static inline unsigned fpm_icl_ma(uint8_t icl_status) {return icl_status * (FPM_CURRENT_STEP_UA / 1000u);}

#ifdef __cplusplus
}
#endif
#endif
