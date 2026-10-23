// SPDX-License-Identifier: BSD-2-Clause-Patent
// SPMI PMIC arbiter v5 core (see Spmi.h). Pure register logic: no mapping, locking or allocation.
#include "Spmi.h"
#include <stdbool.h>

static uint32_t rd(spmi_bus *bus, unsigned window, uint32_t offset) {return bus->io.read32(bus->io.ctx, window, offset);}
static void wr(spmi_bus *bus, unsigned window, uint32_t offset, uint32_t value) {bus->io.write32(bus->io.ctx, window, offset, value);}

int spmi_bus_init(spmi_bus *bus, const spmi_access *io, unsigned ee) {
    if (!bus || !io || !io->read32 || !io->write32 || !io->now_us || ee > 5) return SPMI_EINVAL;
    uint8_t *raw = (uint8_t *)bus;
    for (size_t i = 0; i < sizeof *bus; ++i) raw[i] = 0;
    bus->io = *io;
    bus->ee = (uint8_t)ee;
    bus->version = rd(bus, SPMI_WIN_CORE, SPMI_REG_VERSION);
    if (bus->version < SPMI_VERSION_V5 || bus->version >= SPMI_VERSION_V7) return SPMI_ENOTSUP;
    uint32_t count = rd(bus, SPMI_WIN_CORE, SPMI_REG_FEATURES) & 0x7ffu;
    uint32_t window = (SPMI_CORE_SIZE - SPMI_APID_MAP(0)) / 4u;   // map entries inside the core window
    if (count > window) count = window;
    if (count > SPMI_MAX_APIDS) count = SPMI_MAX_APIDS;
    bus->apid_count = count;
    for (unsigned apid = 0; apid < SPMI_MAX_APIDS; ++apid) bus->apid_irq_ee[apid] = bus->apid_write_ee[apid] = SPMI_INVALID_EE;
    // Several APIDs may map one PPID (one per EE that may write it). Take the one this EE owns, else the first; the highest APID
    // with the IRQ-owner bit gives the interrupt owner (kept for the record, interrupts are not used here).
    for (unsigned apid = 0; apid < count; ++apid) {
        uint32_t map = rd(bus, SPMI_WIN_CORE, SPMI_APID_MAP(apid));
        if (!map) continue;
        unsigned ppid = (map >> 8) & 0xfffu;
        bool irq_owner = (map >> 24) & 1u;
        uint8_t owner = (uint8_t)(rd(bus, SPMI_WIN_CNFG, SPMI_APID_OWNER(apid)) & 0x7u);
        bus->apid_write_ee[apid] = owner;
        bus->apid_irq_ee[apid] = irq_owner ? owner : SPMI_INVALID_EE;
        bus->apid_ppid[apid] = (uint16_t)ppid;
        uint16_t prev = bus->ppid_to_apid[ppid];
        bool valid = prev & SPMI_APID_VALID;
        unsigned prev_apid = prev & ~SPMI_APID_VALID;
        if (!valid || owner == bus->ee) bus->ppid_to_apid[ppid] = (uint16_t)(apid | SPMI_APID_VALID);
        else if (irq_owner && bus->apid_write_ee[prev_apid] == bus->ee) bus->apid_irq_ee[prev_apid] = bus->apid_irq_ee[apid];
        bus->last_apid = (uint16_t)apid;
        ++bus->mapped;
    }
    for (unsigned ppid = 0; ppid < SPMI_MAX_PPIDS; ++ppid) {
        uint16_t v = bus->ppid_to_apid[ppid];
        if ((v & SPMI_APID_VALID) && bus->apid_write_ee[v & ~SPMI_APID_VALID] == bus->ee) ++bus->writable;
    }
    return bus->mapped ? 0 : SPMI_ENODEV;
}

int spmi_bus_apid(const spmi_bus *bus, unsigned sid, unsigned addr) {
    if (!bus || sid > 15 || addr > 0xffff) return SPMI_EINVAL;
    uint16_t v = bus->ppid_to_apid[SPMI_PPID(sid, addr)];
    return (v & SPMI_APID_VALID) ? (int)(v & ~SPMI_APID_VALID) : SPMI_ENODEV;
}

int spmi_bus_can_write(const spmi_bus *bus, unsigned sid, unsigned addr) {
    int apid = spmi_bus_apid(bus, sid, addr);
    if (apid < 0) return apid;
    return bus->apid_write_ee[apid] == bus->ee;
}

static int check_span(unsigned sid, unsigned addr, unsigned len) {
    if (sid > 15 || addr > 0xffff || len < 1 || len > SPMI_MAX_BYTES) return SPMI_EINVAL;
    if (((addr & 0xffu) + len) > 0x100u) return SPMI_EINVAL;   // one channel is one peripheral
    return 0;
}

// Polls STATUS of a channel until done (pmic_arb_wait_for_done): up to SPMI_TIMEOUT_US, at least 64 reads.
static int wait_done(spmi_bus *bus, unsigned window, uint32_t channel) {
    uint64_t start = bus->io.now_us(bus->io.ctx);
    uint32_t status = 0;
    for (unsigned polls = 0;; ++polls) {
        status = rd(bus, window, channel + SPMI_CH_STATUS);
        if (status & SPMI_ST_DONE) break;
        if (polls >= 64 && bus->io.now_us(bus->io.ctx) - start > SPMI_TIMEOUT_US) {
            bus->last_status = status; ++bus->timeouts; ++bus->errors;
            return SPMI_ETIMEDOUT;
        }
        if (polls > 4000000u) {bus->last_status = status; ++bus->timeouts; ++bus->errors; return SPMI_ETIMEDOUT;}   // a stopped clock
    }
    if (status & SPMI_ST_DENIED) {bus->last_status = status; ++bus->denied; ++bus->errors; return SPMI_EPERM;}
    if (status & (SPMI_ST_FAILURE | SPMI_ST_DROPPED)) {bus->last_status = status; ++bus->errors; return SPMI_EIO;}
    return 0;
}

static uint32_t command(unsigned opcode, unsigned addr, unsigned len) {
    return (opcode << 27) | ((addr & 0xffu) << 4) | ((len - 1u) & 0x7u);
}

int spmi_bus_read(spmi_bus *bus, unsigned sid, unsigned addr, uint8_t *buf, unsigned len) {
    if (!bus || !buf) return SPMI_EINVAL;
    int rc = check_span(sid, addr, len);
    if (rc) return rc;
    int apid = spmi_bus_apid(bus, sid, addr);
    if (apid < 0) return apid;
    uint32_t channel = SPMI_OBS_CHAN(bus->ee, (unsigned)apid);
    wr(bus, SPMI_WIN_OBS, channel + SPMI_CH_CMD, command(SPMI_OP_EXT_READL, addr, len));
    rc = wait_done(bus, SPMI_WIN_OBS, channel);
    if (rc) return rc;
    uint32_t data[2] = {rd(bus, SPMI_WIN_OBS, channel + SPMI_CH_RDATA0), 0};
    if (len > 4) data[1] = rd(bus, SPMI_WIN_OBS, channel + SPMI_CH_RDATA1);
    for (unsigned i = 0; i < len; ++i) buf[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
    ++bus->reads;
    return 0;
}

int spmi_bus_write(spmi_bus *bus, unsigned sid, unsigned addr, const uint8_t *buf, unsigned len) {
    if (!bus || !buf) return SPMI_EINVAL;
    int rc = check_span(sid, addr, len);
    if (rc) return rc;
    int apid = spmi_bus_apid(bus, sid, addr);
    if (apid < 0) return apid;
    if (bus->apid_write_ee[apid] != bus->ee) {++bus->denied; return SPMI_EPERM;}   // not this EE's channel: never issued
    uint32_t data[2] = {0, 0};
    for (unsigned i = 0; i < len; ++i) data[i / 4] |= (uint32_t)buf[i] << (8 * (i % 4));
    uint32_t channel = SPMI_RW_CHAN((unsigned)apid);
    wr(bus, SPMI_WIN_CHNLS, channel + SPMI_CH_WDATA0, data[0]);
    if (len > 4) wr(bus, SPMI_WIN_CHNLS, channel + SPMI_CH_WDATA1, data[1]);
    wr(bus, SPMI_WIN_CHNLS, channel + SPMI_CH_CMD, command(SPMI_OP_EXT_WRITEL, addr, len));
    rc = wait_done(bus, SPMI_WIN_CHNLS, channel);
    if (!rc) ++bus->writes;
    return rc;
}

int spmi_write_policy(unsigned sid, unsigned addr, unsigned len) {
    if (len < 1 || len > SPMI_MAX_BYTES) return SPMI_EINVAL;
    unsigned first = addr, last = addr + len - 1u;
    if (((first >> 8) & 0xffu) == 0x08u || ((last >> 8) & 0xffu) == 0x08u) return SPMI_EPERM;   // PON, any PMIC
    if (sid == 2) {
        if (first <= 0x1070u && last >= 0x1070u) return SPMI_EPERM;                         // FLOAT_VOLTAGE_CFG
        if (first <= 0x109fu && last >= 0x1090u) return SPMI_EPERM;                         // JEITA_EN_CFG and thresholds
    }
    return 0;
}
