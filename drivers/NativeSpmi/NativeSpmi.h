// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel's SPMI bus on SDM845 (PMIC arbiter v5 at 0x0c440000, apps EE 0): byte reads and writes of PMIC registers by slave ID
// and 16-bit address. Defined by libNativeSpmi.a (scripts/build-native-spmi.py, NativeSpmi.cpp over the core in Spmi.c).
//
// Slave IDs on fajita: 0 and 1 PM8998 (0: PON 0x800, RTC 0x6000/0x6100, GPIOs 0xc000 + 0x100*(n-1)), 2 and 3 PMI8998
// (2: charger 0x1000..0x16ff, fuel gauge 0x4000.., RRADC 0x4500).
//
// Contract:
// - Any context with interrupts enabled or disabled, after the VM is up. The first call maps the arbiter (ml_io_map) and reads
//   its APID table, so make that first call (native_spmi_init) from a thread; later calls never block or allocate.
// - Transactions are serialised by one spin lock taken with interrupts disabled; each waits at most 1 ms for the arbiter.
// - len is 1..8 and the bytes stay inside one 256-byte peripheral (addr & 0xff00).
// - Return 0 or a negative XNU errno: -EINVAL arguments, -ENODEV no channel maps that peripheral, -EPERM the peripheral is not
//   writable by this EE, refused by the write policy (any PON block, PMI8998 float voltage and JEITA), or the boot argument
//   native-spmi-ro=1 is set, -ETIMEDOUT the arbiter did not finish, -EIO the arbiter reported failure, -ENOTSUP not a v5/v6
//   arbiter. native_spmi_read8 returns the byte (0..255) or the negative errno.
#ifndef NATIVE_SPMI_API_H
#define NATIVE_SPMI_API_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int native_spmi_init(void);
int native_spmi_read(unsigned sid, unsigned addr, void *buf, unsigned len);
int native_spmi_write(unsigned sid, unsigned addr, const void *buf, unsigned len);
int native_spmi_read8(unsigned sid, unsigned addr);
int native_spmi_write8(unsigned sid, unsigned addr, unsigned value);
// Read, replace the bits of mask with those of value, write back, under the lock (pmic_arb_masked_write).
int native_spmi_masked_write(unsigned sid, unsigned addr, unsigned mask, unsigned value);
// APID of (sid, addr's peripheral) and its owner EE; -ENODEV when unmapped.
int native_spmi_channel(unsigned sid, unsigned addr, unsigned *owner_ee);

// Diagnostics: [0] state (0 off, 1 initialising, 2 up, 3 failed), [1] arbiter version, [2] APIDs in the map, [3] PPIDs writable by
// EE 0, [4] reads, [5] writes, [6] errors, [7] last error (negative errno), [8] last bad STATUS, [9] writes refused by policy.
extern volatile uint32_t native_spmi_stats[10];
extern char native_spmi_row[160];

#ifdef __cplusplus
}
#endif
#endif
