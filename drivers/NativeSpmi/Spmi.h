// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm SPMI PMIC arbiter, version 5 (SDM845), register-level core: the APID map, observer-channel reads and owned-channel
// writes by slave ID and 16-bit register address. Register layout and sequences follow the study of the Linux driver
// drivers/spmi/spmi-pmic-arb.c (v6.16: pmic_arb_v5 ops, pmic_arb_read_apid_map_v5, pmic_arb_offset_v5); no code copied.
// No board addresses here: the caller supplies 32-bit register accessors for the four windows and a microsecond clock, and owns
// the mapping and the lock. Host-tested by tests/spmi-test.cpp against a register model of the arbiter and two PMICs.
//
// Windows (sdm845.dtsi spmi@c440000, reg-names):
//   core   0x0c440000  VERSION 0x0, FEATURES 0x4 (bits 10:0 APID count), APID map 0x900 + 4*apid (bits 19:8 PPID, bit 24 IRQ owner)
//   cnfg   0x0c40a000  ownership table 0x700 + 4*apid (bits 2:0 owner EE)
//   obsrvr 0x0e600000  read channel of apid for EE e at 0x10000*e + 0x80*apid
//   chnls  0x0c600000  write channel of apid at 0x10000*apid (only the owner EE may write)
// Channel registers: CMD 0x00, STATUS 0x08 (bit 0 done, 1 failure, 2 denied, 3 dropped), WDATA0/1 0x10/0x14, RDATA0/1 0x18/0x1c.
// Command word (v2 and later): opcode << 27 | (addr & 0xff) << 4 | (bytes - 1); the channel selects the SID and peripheral.
#ifndef NATIVE_SPMI_H
#define NATIVE_SPMI_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {SPMI_WIN_CORE = 0, SPMI_WIN_CNFG = 1, SPMI_WIN_OBS = 2, SPMI_WIN_CHNLS = 3};

#define SPMI_CORE_PHYS 0x0c440000ull
#define SPMI_CORE_SIZE 0x1100u
#define SPMI_CNFG_PHYS 0x0c40a000ull
#define SPMI_OBS_PHYS 0x0e600000ull
#define SPMI_CHNLS_PHYS 0x0c600000ull

#define SPMI_REG_VERSION 0x0
#define SPMI_REG_FEATURES 0x4
#define SPMI_APID_MAP(n) (0x900u + 4u * (n))
#define SPMI_APID_OWNER(n) (0x700u + 4u * (n))
#define SPMI_OBS_CHAN(ee, apid) (0x10000u * (ee) + 0x80u * (apid))
#define SPMI_RW_CHAN(apid) (0x10000u * (apid))
#define SPMI_CH_CMD 0x00
#define SPMI_CH_STATUS 0x08
#define SPMI_CH_WDATA0 0x10
#define SPMI_CH_WDATA1 0x14
#define SPMI_CH_RDATA0 0x18
#define SPMI_CH_RDATA1 0x1c

#define SPMI_ST_DONE 0x1u
#define SPMI_ST_FAILURE 0x2u
#define SPMI_ST_DENIED 0x4u
#define SPMI_ST_DROPPED 0x8u

#define SPMI_OP_EXT_WRITEL 0u
#define SPMI_OP_EXT_READL 1u

#define SPMI_VERSION_V5 0x50000000u
#define SPMI_VERSION_V7 0x70000000u
#define SPMI_MAX_APIDS 512u
#define SPMI_MAX_PPIDS 4096u
#define SPMI_MAX_BYTES 8u
#define SPMI_TIMEOUT_US 1000u
#define SPMI_INVALID_EE 0xffu

// Negative XNU errno values.
#define SPMI_EPERM (-1)
#define SPMI_EIO (-5)
#define SPMI_EBUSY (-16)
#define SPMI_ENODEV (-19)
#define SPMI_EINVAL (-22)
#define SPMI_ENOTSUP (-45)
#define SPMI_ETIMEDOUT (-60)

typedef struct spmi_access {
    uint32_t (*read32)(void *ctx, unsigned window, uint32_t offset);
    void (*write32)(void *ctx, unsigned window, uint32_t offset, uint32_t value);
    uint64_t (*now_us)(void *ctx);
    void *ctx;
} spmi_access;

typedef struct spmi_bus {
    spmi_access io;
    uint32_t version;
    uint32_t apid_count;            // FEATURES[10:0], clipped to the core window and SPMI_MAX_APIDS
    uint8_t ee;                     // this processor's execution environment (apps: 0)
    uint16_t ppid_to_apid[SPMI_MAX_PPIDS];   // bit 15 valid
    uint16_t apid_ppid[SPMI_MAX_APIDS];
    uint8_t apid_write_ee[SPMI_MAX_APIDS];
    uint8_t apid_irq_ee[SPMI_MAX_APIDS];
    uint16_t mapped;                // APIDs with a non-zero map entry
    uint16_t writable;              // PPIDs whose chosen APID this EE owns
    uint16_t last_apid;
    uint32_t last_status;           // STATUS of the last transaction that did not end done-and-clean
    uint32_t reads, writes, errors, timeouts, denied;
} spmi_bus;

#define SPMI_PPID(sid, addr) ((((unsigned)(sid) & 0xfu) << 8) | (((unsigned)(addr) >> 8) & 0xffu))
#define SPMI_APID_VALID 0x8000u

// Reads VERSION and FEATURES and the APID map (pmic_arb_read_apid_map_v5). Version 5 and 6 only; v7 has another layout.
int spmi_bus_init(spmi_bus *bus, const spmi_access *io, unsigned ee);
// APID chosen for (sid, peripheral) or SPMI_ENODEV.
int spmi_bus_apid(const spmi_bus *bus, unsigned sid, unsigned addr);
// 1 when this EE may write the peripheral of addr, 0 when not, negative when it is not mapped.
int spmi_bus_can_write(const spmi_bus *bus, unsigned sid, unsigned addr);
// 1..8 bytes from consecutive registers of one peripheral (addr..addr+len-1 must not cross a 256-byte peripheral boundary).
int spmi_bus_read(spmi_bus *bus, unsigned sid, unsigned addr, uint8_t *buf, unsigned len);
int spmi_bus_write(spmi_bus *bus, unsigned sid, unsigned addr, const uint8_t *buf, unsigned len);

// Board write policy of this tree, applied by the kernel binding before spmi_bus_write: 0 allowed, SPMI_EPERM refused.
// Refused on every SID: the PON peripheral (0x08xx: PS_HOLD, S1/S2/S3 reset and shutdown control). On the PMI8998 (SID 2):
// the charger's float voltage (0x1070) and JEITA configuration (0x1090..0x109f).
int spmi_write_policy(unsigned sid, unsigned addr, unsigned len);

#ifdef __cplusplus
}
#endif
#endif
