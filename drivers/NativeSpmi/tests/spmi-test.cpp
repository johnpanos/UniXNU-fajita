// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the SPMI arbiter v5 core against a register model: an arbiter with an APID map, an ownership table, observer and
// read/write channels that execute EXT_READL/EXT_WRITEL against two modelled PMIC register files.
#include "Spmi.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

namespace {
int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Model {
    uint32_t version = 0x50000000, features = 0;
    std::map<unsigned, uint32_t> apidMap, owner;          // apid -> map word / owner EE
    uint8_t pmic[16][0x10000] = {};                        // register files per SID
    std::map<uint32_t, uint32_t> chan[4];                  // window -> offset -> value
    bool hang = false, deny = false;
    unsigned commands = 0, writeCommands = 0;
    uint64_t clock = 0;

    void map(unsigned apid, unsigned sid, unsigned periph, unsigned ee, bool irq = false) {
        apidMap[apid] = ((sid << 8 | periph) << 8) | (irq ? 1u << 24 : 0);
        owner[apid] = ee;
        if (apid + 1 > features) features = apid + 1;
    }
    unsigned ppidOf(unsigned apid) const {auto it = apidMap.find(apid); return it == apidMap.end() ? 0 : (it->second >> 8) & 0xfff;}

    uint32_t read(unsigned w, uint32_t off) {
        if (w == SPMI_WIN_CORE) {
            if (off == SPMI_REG_VERSION) return version;
            if (off == SPMI_REG_FEATURES) return features;
            if (off >= 0x900) {auto it = apidMap.find((off - 0x900) / 4); return it == apidMap.end() ? 0 : it->second;}
            return 0;
        }
        if (w == SPMI_WIN_CNFG) {
            if (off >= 0x700 && off < 0xb00) {auto it = owner.find((off - 0x700) / 4); return it == owner.end() ? 0 : it->second;}
            return 0;
        }
        auto &m = chan[w];
        auto it = m.find(off);
        return it == m.end() ? 0 : it->second;
    }
    void write(unsigned w, uint32_t off, uint32_t v) {
        chan[w][off] = v;
        if ((off & 0x7f) != SPMI_CH_CMD || (w != SPMI_WIN_OBS && w != SPMI_WIN_CHNLS)) return;
        ++commands;
        unsigned apid = w == SPMI_WIN_OBS ? (off & 0xffff) / 0x80 : off >> 16;
        uint32_t base = off;
        unsigned ppid = ppidOf(apid), sid = ppid >> 8, periph = ppid & 0xff;
        unsigned opc = v >> 27, addr = (periph << 8) | ((v >> 4) & 0xff), len = (v & 7) + 1;
        if (hang) {chan[w][base + SPMI_CH_STATUS] = 0; return;}
        if (deny) {chan[w][base + SPMI_CH_STATUS] = SPMI_ST_DONE | SPMI_ST_DENIED; return;}
        if (w == SPMI_WIN_OBS && opc == SPMI_OP_EXT_READL) {
            uint32_t d[2] = {0, 0};
            for (unsigned i = 0; i < len; ++i) d[i / 4] |= (uint32_t)pmic[sid][addr + i] << (8 * (i % 4));
            chan[w][base + SPMI_CH_RDATA0] = d[0];
            chan[w][base + SPMI_CH_RDATA1] = d[1];
        } else if (w == SPMI_WIN_CHNLS && opc == SPMI_OP_EXT_WRITEL) {
            ++writeCommands;
            uint32_t d[2] = {chan[w][base + SPMI_CH_WDATA0], chan[w][base + SPMI_CH_WDATA1]};
            for (unsigned i = 0; i < len; ++i) pmic[sid][addr + i] = (uint8_t)(d[i / 4] >> (8 * (i % 4)));
        } else {
            chan[w][base + SPMI_CH_STATUS] = SPMI_ST_DONE | SPMI_ST_FAILURE;
            return;
        }
        chan[w][base + SPMI_CH_STATUS] = SPMI_ST_DONE;
    }
};

Model *gModel;
uint32_t rd32(void *, unsigned w, uint32_t off) {return gModel->read(w, off);}
void wr32(void *, unsigned w, uint32_t off, uint32_t v) {gModel->write(w, off, v);}
uint64_t now(void *) {return gModel->clock += 3;}

spmi_bus *freshBus(Model &m) {
    gModel = &m;
    static spmi_bus bus;
    spmi_access io = {rd32, wr32, now, nullptr};
    CHECK(spmi_bus_init(&bus, &io, 0) == 0);
    return &bus;
}

void testMap() {
    Model *m = new Model;
    m->map(0, 0, 0x08, 0);          // PM8998 PON
    m->map(3, 0, 0x60, 1);          // PM8998 RTC first mapped for EE 1 ...
    m->map(4, 0, 0x60, 0, true);    // ... and again for EE 0 (taken)
    m->map(7, 2, 0x10, 0);          // PMI8998 CHGR
    m->map(9, 2, 0x40, 3);          // PMI8998 FG owned by another EE only
    spmi_bus *bus = freshBus(*m);
    CHECK(bus->mapped == 5);
    CHECK(spmi_bus_apid(bus, 0, 0x0810) == 0);
    CHECK(spmi_bus_apid(bus, 0, 0x6048) == 4);
    CHECK(spmi_bus_apid(bus, 2, 0x1042) == 7);
    CHECK(spmi_bus_apid(bus, 2, 0x4009) == 9);
    CHECK(spmi_bus_apid(bus, 1, 0x0810) == SPMI_ENODEV);
    CHECK(spmi_bus_can_write(bus, 2, 0x4009) == 0);
    CHECK(spmi_bus_can_write(bus, 0, 0x6048) == 1);
    CHECK(bus->writable == 3);
    CHECK(bus->apid_irq_ee[4] == 0);
    delete m;
}

void testReadWrite() {
    Model *m = new Model;
    m->map(0, 0, 0x08, 0);
    m->map(7, 2, 0x10, 0);
    m->map(9, 2, 0x40, 3);
    for (unsigned i = 0; i < 0x100; ++i) m->pmic[2][0x4000 + i] = (uint8_t)(i ^ 0x5a);
    m->pmic[0][0x0810] = 0x01;
    spmi_bus *bus = freshBus(*m);
    uint8_t b[8] = {};
    CHECK(spmi_bus_read(bus, 0, 0x0810, b, 1) == 0 && b[0] == 0x01);
    CHECK(spmi_bus_read(bus, 2, 0x4009, b, 2) == 0 && b[0] == (0x09 ^ 0x5a) && b[1] == (0x0a ^ 0x5a));
    CHECK(spmi_bus_read(bus, 2, 0x4010, b, 8) == 0);
    for (unsigned i = 0; i < 8; ++i) CHECK(b[i] == ((0x10 + i) ^ 0x5a));
    // Read channel: observer window of EE 0 at 0x80 * apid.
    CHECK(m->chan[SPMI_WIN_OBS].count(SPMI_OBS_CHAN(0, 9) + SPMI_CH_CMD));
    const uint8_t w[6] = {1, 2, 3, 4, 5, 6};
    CHECK(spmi_bus_write(bus, 2, 0x1060, w, 6) == 0);
    CHECK(std::memcmp(&m->pmic[2][0x1060], w, 6) == 0);
    CHECK(m->chan[SPMI_WIN_CHNLS].count(SPMI_RW_CHAN(7) + SPMI_CH_CMD));
    // Not this EE's channel: refused without a command.
    unsigned before = m->commands;
    CHECK(spmi_bus_write(bus, 2, 0x4050, w, 1) == SPMI_EPERM);
    CHECK(m->commands == before);
    // Spans and arguments.
    CHECK(spmi_bus_read(bus, 2, 0x40fe, b, 4) == SPMI_EINVAL);
    CHECK(spmi_bus_read(bus, 2, 0x4000, b, 0) == SPMI_EINVAL);
    CHECK(spmi_bus_read(bus, 2, 0x4000, b, 9) == SPMI_EINVAL);
    CHECK(spmi_bus_read(bus, 16, 0x4000, b, 1) == SPMI_EINVAL);
    CHECK(spmi_bus_read(bus, 5, 0x4000, b, 1) == SPMI_ENODEV);
    // Command word: EXT_READL, low address byte, byte count - 1.
    spmi_bus_read(bus, 2, 0x40a7, b, 3);
    CHECK(m->chan[SPMI_WIN_OBS][SPMI_OBS_CHAN(0, 9) + SPMI_CH_CMD] == ((1u << 27) | (0xa7u << 4) | 2u));
    // Hung and denying arbiter.
    m->hang = true;
    CHECK(spmi_bus_read(bus, 2, 0x4009, b, 1) == SPMI_ETIMEDOUT);
    CHECK(bus->timeouts == 1);
    m->hang = false;
    m->deny = true;
    CHECK(spmi_bus_read(bus, 2, 0x4009, b, 1) == SPMI_EPERM);
    m->deny = false;
    CHECK(spmi_bus_read(bus, 2, 0x4009, b, 1) == 0);
    delete m;
}

void testInitRejects() {
    Model *m = new Model;
    m->version = 0x30000000;
    m->map(0, 0, 0x08, 0);
    gModel = m;
    spmi_bus bus;
    spmi_access io = {rd32, wr32, now, nullptr};
    CHECK(spmi_bus_init(&bus, &io, 0) == SPMI_ENOTSUP);
    m->version = 0x70000000;
    CHECK(spmi_bus_init(&bus, &io, 0) == SPMI_ENOTSUP);
    Model *empty = new Model;
    gModel = empty;
    CHECK(spmi_bus_init(&bus, &io, 0) == SPMI_ENODEV);
    CHECK(spmi_bus_init(&bus, &io, 6) == SPMI_EINVAL);
    // FEATURES larger than the core window: clipped to (0x1100 - 0x900) / 4 entries.
    empty->features = 0x7ff;
    empty->map(511, 0, 0x08, 0);
    empty->features = 0x7ff;
    CHECK(spmi_bus_init(&bus, &io, 0) == 0 && bus.apid_count == 512);
    delete m;
    delete empty;
}

void testPolicy() {
    CHECK(spmi_write_policy(0, 0x0810, 1) == SPMI_EPERM);
    CHECK(spmi_write_policy(0, 0x085a, 1) == SPMI_EPERM);
    CHECK(spmi_write_policy(2, 0x0880, 1) == SPMI_EPERM);
    CHECK(spmi_write_policy(0, 0x07fe, 4) == SPMI_EPERM);   // runs into PON
    CHECK(spmi_write_policy(2, 0x1070, 1) == SPMI_EPERM);
    CHECK(spmi_write_policy(2, 0x106e, 4) == SPMI_EPERM);
    CHECK(spmi_write_policy(2, 0x1090, 1) == SPMI_EPERM);
    CHECK(spmi_write_policy(2, 0x109f, 1) == SPMI_EPERM);
    CHECK(spmi_write_policy(2, 0x1042, 1) == 0);
    CHECK(spmi_write_policy(2, 0x1061, 1) == 0);
    CHECK(spmi_write_policy(2, 0x1370, 1) == 0);
    CHECK(spmi_write_policy(0, 0x6040, 4) == 0);            // RTC
    CHECK(spmi_write_policy(0, 0x1070, 1) == 0);            // only the PMI8998's float voltage
    CHECK(spmi_write_policy(0, 0x6040, 0) == SPMI_EINVAL);
}
}

int main() {
    testMap();
    testReadWrite();
    testInitRejects();
    testPolicy();
    if (failures) {std::printf("spmi-test: %d failures\n", failures); return 1;}
    std::printf("spmi-test: ok\n");
    return 0;
}
