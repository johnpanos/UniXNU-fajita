// SPDX-License-Identifier: BSD-2-Clause-Patent
// Kernel binding of the SPMI arbiter core (Spmi.c): maps the windows with ml_io_map, serialises transactions, applies the board
// write policy and exports the C API of NativeSpmi.h. Nothing is mapped or read before the first call.
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>
#include <string.h>

#include "Spmi.h"
#include "NativeSpmi.h"

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
volatile uint32_t native_spmi_stats[10];
char native_spmi_row[160];
}

namespace {
constexpr unsigned kEe = 0;              // apps processor (sdm845.dtsi qcom,ee = <0>)
constexpr uint32_t kCnfgMap = 0x1000;    // the ownership table ends at 0x700 + 4 * 511
spmi_bus gBus;
uintptr_t gCore, gCnfg, gObs;
uintptr_t gChan[SPMI_MAX_APIDS];         // write channel page of each APID this EE owns
IOSimpleLock *gLock;
volatile int gState;                     // 0 off, 1 initialising, 2 up, 3 failed
int gInitError;
bool gReadOnly;

uint64_t ticks() {uint64_t t; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(t) :: "memory"); return t;}
uint64_t tickHz() {uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 19200000;}

uintptr_t windowAddress(unsigned window, uint32_t offset) {
    switch (window) {
    case SPMI_WIN_CORE: return offset < 0x2000 ? gCore + offset : 0;
    case SPMI_WIN_CNFG: return offset < kCnfgMap ? gCnfg + offset : 0;
    case SPMI_WIN_OBS: {
        uint32_t base = SPMI_OBS_CHAN(kEe, 0);
        return offset >= base && offset - base < 0x10000 ? gObs + (offset - base) : 0;
    }
    case SPMI_WIN_CHNLS: {
        uint32_t apid = offset >> 16;
        return apid < SPMI_MAX_APIDS && gChan[apid] && (offset & 0xffff) < 0x1000 ? gChan[apid] + (offset & 0xfff) : 0;
    }
    }
    return 0;
}

uint32_t read32(void *, unsigned window, uint32_t offset) {
    uintptr_t va = windowAddress(window, offset);
    return va ? *reinterpret_cast<volatile uint32_t *>(va) : 0;
}

void write32(void *, unsigned window, uint32_t offset, uint32_t value) {
    uintptr_t va = windowAddress(window, offset);
    if (!va) return;
    __asm__ volatile("dsb st" ::: "memory");     // data words reach the channel before the command word
    *reinterpret_cast<volatile uint32_t *>(va) = value;
}

uint64_t nowUs(void *) {return ticks() * 1000000ull / tickHz();}

void summarise() {
    native_spmi_stats[0] = (uint32_t)gState;
    native_spmi_stats[1] = gBus.version;
    native_spmi_stats[2] = gBus.mapped;
    native_spmi_stats[3] = gBus.writable;
    native_spmi_stats[4] = gBus.reads;
    native_spmi_stats[5] = gBus.writes;
    native_spmi_stats[6] = gBus.errors;
    native_spmi_stats[8] = gBus.last_status;
    snprintf(native_spmi_row, sizeof native_spmi_row, "SPMI st%d e%d v%08x apids%u/%u w%u rd%u wr%u err%u to%u dn%u pol%u last%d/%x%s",
             gState, gInitError, gBus.version, gBus.mapped, gBus.apid_count, gBus.writable, gBus.reads, gBus.writes, gBus.errors,
             gBus.timeouts, gBus.denied, native_spmi_stats[9], (int)native_spmi_stats[7], gBus.last_status, gReadOnly ? " ro" : "");
}

int fail(int rc) {
    if (rc < 0) native_spmi_stats[7] = (uint32_t)rc;
    return rc;
}

int doInit() {
    unsigned ro = 0;
    gReadOnly = PE_parse_boot_argn("native-spmi-ro", &ro, sizeof ro) && ro;
    gLock = IOSimpleLockAlloc();
    gCore = ml_io_map((vm_offset_t)SPMI_CORE_PHYS, 0x2000);
    gCnfg = ml_io_map((vm_offset_t)SPMI_CNFG_PHYS, kCnfgMap);
    gObs = ml_io_map((vm_offset_t)(SPMI_OBS_PHYS + SPMI_OBS_CHAN(kEe, 0)), 0x10000);
    if (!gLock || !gCore || !gCnfg || !gObs) return SPMI_EIO;
    spmi_access io = {read32, write32, nowUs, nullptr};
    int rc = spmi_bus_init(&gBus, &io, kEe);
    if (rc) return rc;
    // One page per APID this EE owns: 0x10000 * apid into chnls (pmic_arb_offset_v5); nothing else is ever written.
    for (unsigned apid = 0; apid < gBus.apid_count; ++apid) {
        if (gBus.apid_write_ee[apid] != kEe) continue;   // unmapped entries hold SPMI_INVALID_EE
        gChan[apid] = ml_io_map((vm_offset_t)(SPMI_CHNLS_PHYS + SPMI_RW_CHAN(apid)), 0x1000);
    }
    return 0;
}

// The lock with interrupts disabled around one transaction (pmic_arb_read_cmd/write_cmd hold a raw spin lock the same way).
struct Locked {
    IOInterruptState is;
    Locked() : is(IOSimpleLockLockDisableInterrupt(gLock)) {}
    ~Locked() {IOSimpleLockUnlockEnableInterrupt(gLock, is);}
};
}

extern "C" int native_spmi_init(void) {
    int state = __atomic_load_n(&gState, __ATOMIC_ACQUIRE);
    if (state == 2) return 0;
    if (state == 3) return fail(gInitError);
    int expected = 0;
    if (__atomic_compare_exchange_n(&gState, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        gInitError = doInit();
        __atomic_store_n(&gState, gInitError ? 3 : 2, __ATOMIC_RELEASE);
        summarise();
        if (gInitError) IOLog("native-spmi: init failed %d (%s)\n", gInitError, native_spmi_row);
        else IOLog("native-spmi: %s\n", native_spmi_row);
        return fail(gInitError);
    }
    // Another caller is mapping: wait for it (at most ~1 s; a caller with interrupts off spins without sleeping).
    for (unsigned i = 0; i < 1000000 && __atomic_load_n(&gState, __ATOMIC_ACQUIRE) == 1; ++i) IODelay(1);
    state = __atomic_load_n(&gState, __ATOMIC_ACQUIRE);
    return state == 2 ? 0 : fail(state == 3 ? gInitError : SPMI_EBUSY);
}

extern "C" int native_spmi_read(unsigned sid, unsigned addr, void *buf, unsigned len) {
    int rc = native_spmi_init();
    if (rc) return rc;
    {
        Locked l;
        rc = spmi_bus_read(&gBus, sid, addr, static_cast<uint8_t *>(buf), len);
    }
    if (rc) {fail(rc); summarise();}
    else native_spmi_stats[4] = gBus.reads;
    return rc;
}

extern "C" int native_spmi_write(unsigned sid, unsigned addr, const void *buf, unsigned len) {
    int rc = native_spmi_init();
    if (rc) return rc;
    if (gReadOnly || (rc = spmi_write_policy(sid, addr, len)) != 0) {
        native_spmi_stats[9] = native_spmi_stats[9] + 1;
        summarise();
        return fail(SPMI_EPERM);
    }
    {
        Locked l;
        rc = spmi_bus_write(&gBus, sid, addr, static_cast<const uint8_t *>(buf), len);
    }
    if (rc) fail(rc);
    summarise();
    return rc;
}

extern "C" int native_spmi_read8(unsigned sid, unsigned addr) {
    uint8_t v = 0;
    int rc = native_spmi_read(sid, addr, &v, 1);
    return rc ? rc : v;
}

extern "C" int native_spmi_write8(unsigned sid, unsigned addr, unsigned value) {
    uint8_t v = (uint8_t)value;
    return native_spmi_write(sid, addr, &v, 1);
}

extern "C" int native_spmi_masked_write(unsigned sid, unsigned addr, unsigned mask, unsigned value) {
    int rc = native_spmi_init();
    if (rc) return rc;
    if (gReadOnly || spmi_write_policy(sid, addr, 1)) {
        native_spmi_stats[9] = native_spmi_stats[9] + 1;
        summarise();
        return fail(SPMI_EPERM);
    }
    {
        Locked l;
        uint8_t v = 0;
        rc = spmi_bus_read(&gBus, sid, addr, &v, 1);
        if (!rc) {
            v = (uint8_t)((v & ~mask) | (value & mask));
            rc = spmi_bus_write(&gBus, sid, addr, &v, 1);
        }
    }
    if (rc) fail(rc);
    summarise();
    return rc;
}

extern "C" int native_spmi_channel(unsigned sid, unsigned addr, unsigned *owner_ee) {
    int rc = native_spmi_init();
    if (rc) return rc;
    int apid = spmi_bus_apid(&gBus, sid, addr);
    if (apid >= 0 && owner_ee) *owner_ee = gBus.apid_write_ee[apid];
    return apid;
}
