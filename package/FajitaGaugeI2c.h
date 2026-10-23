// SPDX-License-Identifier: BSD-2-Clause-Patent
// The GENI I2C engine (drivers/NativeGeniI2c) under fpm_ names for the PMIC archive's own copy (the fuel gauge on i2c10), so it can
// be linked next to libFajitaTouch.a, which carries the same engine for the touch bus (i2c12), without duplicate symbols.
#ifndef FAJITA_GAUGE_I2C_H
#define FAJITA_GAUGE_I2C_H
#define geni_i2c_find_timing fpm_geni_i2c_find_timing
#define geni_i2c_init fpm_geni_i2c_init
#define geni_i2c_packing fpm_geni_i2c_packing
#define geni_i2c_read fpm_geni_i2c_read
#define geni_i2c_read_info fpm_geni_i2c_read_info
#define geni_i2c_recover fpm_geni_i2c_recover
#define geni_i2c_set_clock fpm_geni_i2c_set_clock
#define geni_i2c_write fpm_geni_i2c_write
#define geni_i2c_write_read fpm_geni_i2c_write_read
#include "GeniI2c.h"
#endif
