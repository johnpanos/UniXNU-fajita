// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the fajita PMIC board helpers (FajitaPmicBoard.h).
#include "FajitaPmicBoard.h"
#include <cstdio>
#include <cstring>

namespace {
int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

const fpm_write *find(const fpm_write *seq, unsigned n, unsigned addr) {
    for (unsigned i = 0; i < n; ++i) if (seq[i].addr == addr) return &seq[i];
    return nullptr;
}

void testSequence() {
    fpm_write seq[FPM_CHARGE_SEQ_LEN + 1];
    CHECK(fpm_charge_sequence(seq, FPM_CHARGE_SEQ_LEN, 1000, 1500) == 0);
    unsigned n = fpm_charge_sequence(seq, FPM_CHARGE_SEQ_LEN + 1, 0, 0);
    CHECK(n == FPM_CHARGE_SEQ_LEN + 1);
    CHECK(fpm_sequence_is_safe(seq, n));
    CHECK(find(seq, n, FPM_FAST_CHARGE_CFG)->value == 40);            // 1.0 A default
    CHECK(seq[n - 1].addr == FPM_USBIN_CURRENT_LIMIT_CFG && seq[n - 1].value == 60);   // 1.5 A, last
    CHECK(!find(seq, n, FPM_FLOAT_VOLTAGE_CFG));
    // Clamps: never above the pmOS 1.95 A fast charge or the 1.5 A input limit.
    n = fpm_charge_sequence(seq, FPM_CHARGE_SEQ_LEN + 1, 5000, 3000);
    CHECK(find(seq, n, FPM_FAST_CHARGE_CFG)->value == 78);
    CHECK(seq[n - 1].value == 60);
    CHECK(fpm_sequence_is_safe(seq, n));
    // The pmOS values, entry by entry.
    CHECK(find(seq, n, FPM_CHG_ENABLE_CMD)->mask == 1 && find(seq, n, FPM_CHG_ENABLE_CMD)->value == 1);
    CHECK(find(seq, n, FPM_CHG_CFG2)->mask == 0xff && find(seq, n, FPM_CHG_CFG2)->value == 0x01);
    CHECK(find(seq, n, FPM_USBIN_AICL_OPTIONS_CFG)->mask == 0xaf && find(seq, n, FPM_USBIN_AICL_OPTIONS_CFG)->value == 0x07);
    CHECK(find(seq, n, FPM_WD_CFG)->mask == 0xc2 && find(seq, n, FPM_WD_CFG)->value == 0);
    CHECK(find(seq, n, FPM_PRE_CHARGE_CFG)->value == 20);
    CHECK(find(seq, n, FPM_TYPE_C_INTRPT_ENB_SW_CTRL)->mask == 0x1f && find(seq, n, FPM_TYPE_C_INTRPT_ENB_SW_CTRL)->value == 0x10);
    // Unsafe sequences are caught.
    fpm_write bad[1] = {{FPM_FLOAT_VOLTAGE_CFG, 0xff, 0x7a, "x"}};
    CHECK(!fpm_sequence_is_safe(bad, 1));
    bad[0] = {0x1095, 0xff, 0, "x"};
    CHECK(!fpm_sequence_is_safe(bad, 1));
    bad[0] = {0x0810, 0xff, 0, "x"};
    CHECK(!fpm_sequence_is_safe(bad, 1));
    bad[0] = {FPM_USBIN_CURRENT_LIMIT_CFG, 0xff, 61, "x"};
    CHECK(!fpm_sequence_is_safe(bad, 1));
    bad[0] = {FPM_FAST_CHARGE_CFG, 0xff, 79, "x"};
    CHECK(!fpm_sequence_is_safe(bad, 1));
    CHECK(fpm_apply(0xf0, 0x0f, 0x05) == 0xf5);
    CHECK(fpm_apply(0xff, 0x01, 0x00) == 0xfe);
}

void testKeys() {
    CHECK(fpm_keys_decode(0x00, 1, 1) == 0);
    CHECK(fpm_keys_decode(0x01, 1, 1) == FPM_KEY_POWER);
    CHECK(fpm_keys_decode(0x02, 1, 1) == 0);                          // RESIN is not a key here
    CHECK(fpm_keys_decode(0x00, 0, 1) == FPM_KEY_VOL_DOWN);
    CHECK(fpm_keys_decode(0x00, 1, 0) == FPM_KEY_VOL_UP);
    CHECK(fpm_keys_decode(-60, -60, -60) == 0);                       // failed reads: released
    fpm_debounce d = {};
    CHECK(fpm_debounce_step(&d, 1, 2) == 0);
    CHECK(fpm_debounce_step(&d, 1, 2) == 1);
    CHECK(fpm_debounce_step(&d, 0, 2) == 1);                          // one glitch sample is ignored
    CHECK(fpm_debounce_step(&d, 1, 2) == 1);
    CHECK(fpm_debounce_step(&d, 0, 2) == 1);
    CHECK(fpm_debounce_step(&d, 0, 2) == 0);
    CHECK(fpm_debounce_step(&d, 4, 1) == 4);
}

void testConversions() {
    // 0x7d00 * 122.07 uV = 3.906 V.
    CHECK(fpm_fg_voltage_mv(0x00, 0x7d) == 3906);
    CHECK(fpm_fg_voltage_mv(0, 0) == 0);
    // -2048 LSB * 488.281 uA = -1 A in the PMIC's sign: charging at +1000 mA.
    CHECK(fpm_fg_current_ma(0x00, 0xf8) == 1000);
    CHECK(fpm_fg_current_ma(0x00, 0x04) == -500);
    // 0x4a4 = 1188 quarter-kelvin = 297.0 K = 23.9 C.
    CHECK(fpm_fg_temp_dc(0xa4, 0x04) == 240);
    CHECK(fpm_fg_soc_percent(0xff, 0xff) == 100);
    CHECK(fpm_fg_soc_percent(1, 1) == 1);
    CHECK(fpm_fg_soc_percent(0, 5) == 0);
    CHECK(fpm_fg_soc_percent(0x80, 0x81) == 50);
    CHECK(std::strcmp(fpm_charger_state_name(2), "fast") == 0);
    CHECK(std::strcmp(fpm_charger_state_name(0xf7), "disabled") == 0);
    CHECK(fpm_charging(2, 0, 1) == 1);
    CHECK(fpm_charging(2, 0x20, 1) == 0);
    CHECK(fpm_charging(2, 0, 0) == 0);
    CHECK(fpm_charging(5, 0, 1) == 2);
    CHECK(fpm_charging(7, 0, 1) == 0);
    CHECK(fpm_usb_online(0x11) && !fpm_usb_online(0x10) && !fpm_usb_online(0x01));
    CHECK(fpm_icl_ma(60) == 1500);
}
}

int main() {
    testSequence();
    testKeys();
    testConversions();
    if (failures) {std::printf("fajita-pmic-board-test: %d failures\n", failures); return 1;}
    std::printf("fajita-pmic-board-test: ok\n");
    return 0;
}
