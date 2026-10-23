// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita PMIC bring-up over SPMI (drivers/NativeSpmi): PM8998 (SID 0) and PMI8998 (SID 2). Board constants and the pure logic are
// in FajitaPmicBoard.h. `fajita-pmic=<stage>` (default off: nothing is mapped or read without it):
//   1  read only: arbiter and APID table, PMIC identities, a dump of every register block the later stages read or write
//      (PON, the volume-key GPIOs, the PMI8998 charger blocks, fuel gauge, RRADC), then a status row every period
//   2  additionally program the charger as the pmOS qcom_smbx driver does (FajitaPmicBoard.c, the float voltage excepted) with
//      the input limit at 1.5 A, each write logged with its old value and read back; then the same status rows
// Options: fajita-pmic-period=<s> (10), fajita-pmic-fcc=<mA> (1000, at most 1950), fajita-pmic-icl=<mA> (1500, at most 1500),
// fajita-pmic-dump=0 (skip the register dump).
// `fajita-pmic-keys=1` (independent of the stage; read only): power (PON KPDPWR), volume up (GPIO 6) and volume down (GPIO 5) polled
//   every 20 ms, debounced over two samples, published in native_button_state for NativeHidInput (native-hid-buttons=1).
//   `fajita-pmic-keys-synth=<s>`: unattended test, from <s> seconds after boot synthetic presses through the same publication:
//   volume up, volume down, then power (200 ms each, 3 s apart), `fajita-pmic-keys-synth-count=<n>` rounds (1) 20 s apart;
//   `fajita-pmic-keys-synth-mask=<bits>` limits them to some keys (7: all; bit 0 power, 1 up, 2 down).
// `fajita-pmic-batt=1` (with a stage): the battery as an IOPMPowerSource (class AppleSmartBattery) for powerd, from the PMI8998 fuel
//   gauge and the charger status, refreshed every period.
// Everything runs in its own kernel thread, started from the boot progress monitor (fajita_pmic_poll, once a second) once IOKit is
// up and, with fajita-wdog, after ArmCpu armed the APSS watchdog.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSDictionary.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

#include "NativeSpmi.h"
#include "FajitaPmicBoard.h"

extern "C" {
char fajita_pmic_rows[8][160];
volatile uint32_t fajita_pmic_state[8];   // [0] stage reached, [1] identity ok, [2] writes done, [3] write errors, [4] rows
extern volatile uint32_t armcpu_wdog[6] __attribute__((weak_import));
volatile uint32_t native_button_state;        // bit 0 power, 1 volume up, 2 volume down (debounced real keys | synthetic)
volatile uint32_t fajita_pmic_keys[8];        // [0] polls, [1] read errors, [2] changes, [3] real mask, [4] synthetic presses, [5] sink calls
extern volatile uint32_t native_hid_button_stats[4] __attribute__((weak_import));
kern_return_t kernel_thread_start_priority(thread_continue_t, void *, integer_t, thread_t *);
}

namespace {
unsigned gStage, gPeriod = 10, gFcc, gIcl, gDump = 1, gKeys, gSynth, gSynthCount = 1, gSynthMask = 7, gBatt;
bool gStarted;
void (*gButtonSink)(void *);
void *gButtonSinkContext;

uint64_t ticks() {uint64_t t; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(t) :: "memory"); return t;}
uint64_t tickHz() {uint64_t f; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 19200000;}
unsigned uptime() {return (unsigned)(ticks() / tickHz());}

int rd8(unsigned sid, unsigned addr) {return native_spmi_read8(sid, addr);}
int rd16(unsigned sid, unsigned addr, uint8_t out[2]) {return native_spmi_read(sid, addr, out, 2);}

void logApids() {
    // Every mapped PPID with its owner: "sid.pp:o" (o = owner EE, * = this EE may write).
    char line[200];
    int n = 0, count = 0;
    for (unsigned sid = 0; sid < 4; ++sid)
        for (unsigned pp = 0; pp < 256; ++pp) {
            unsigned owner = 0;
            int apid = native_spmi_channel(sid, pp << 8, &owner);
            if (apid < 0) continue;
            n += snprintf(line + n, sizeof line - n, " %u.%02x:%u%s", sid, pp, owner, owner == 0 ? "*" : "");
            if (++count % 16 == 0 || n > 150) {IOLog("fajita-pmic: ppid%s\n", line); n = 0;}
        }
    if (n) IOLog("fajita-pmic: ppid%s\n", line);
    IOLog("fajita-pmic: %d peripherals mapped\n", count);
}

void dumpBlock(unsigned sid, unsigned base, unsigned len) {
    unsigned owner = 0;
    int apid = native_spmi_channel(sid, base, &owner);
    if (apid < 0) {IOLog("fajita-pmic: dump %u:%04x unmapped (%d)\n", sid, base, apid); return;}
    for (unsigned off = 0; off < len; off += 16) {
        uint8_t b[16];
        int rc = native_spmi_read(sid, base + off, b, 8);
        if (!rc) rc = native_spmi_read(sid, base + off + 8, b + 8, 8);
        if (rc) {IOLog("fajita-pmic: dump %u:%04x err %d\n", sid, base + off, rc); return;}
        IOLog("fajita-pmic: dump %u:%04x a%d o%u %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n",
              sid, base + off, apid, owner, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
              b[14], b[15]);
    }
}

void dumpAll() {
    struct {unsigned sid, base, len;} blocks[] = {
        {0, 0x0100, 0x10}, {2, 0x0100, 0x10},                                       // REVID
        {0, 0x0800, 0x100},                                                         // PM8998 PON (read only, ever)
        {0, FPM_GPIO_BASE(FPM_GPIO_VOL_DOWN), 0x50}, {0, FPM_GPIO_BASE(FPM_GPIO_VOL_UP), 0x50},
        {2, 0x1000, 0x100}, {2, 0x1100, 0x100}, {2, 0x1200, 0x100}, {2, 0x1300, 0x100}, {2, 0x1400, 0x100}, {2, 0x1600, 0x100},
        {2, 0x4000, 0x100}, {2, 0x4100, 0x100}, {2, 0x4500, 0x100},
    };
    for (auto &b : blocks) dumpBlock(b.sid, b.base, b.len);
}

bool identity() {
    int t0 = rd8(0, FPM_REVID_TYPE), s0 = rd8(0, FPM_REVID_SUBTYPE), t2 = rd8(2, FPM_REVID_TYPE), s2 = rd8(2, FPM_REVID_SUBTYPE);
    int r0 = rd8(0, FPM_REVID_REV4), r2 = rd8(2, FPM_REVID_REV4);
    bool ok = t0 == FPM_PMIC_TYPE && s0 == FPM_SUBTYPE_PM8998 && t2 == FPM_PMIC_TYPE && s2 == FPM_SUBTYPE_PMI8998;
    snprintf(fajita_pmic_rows[1], sizeof fajita_pmic_rows[1], "[pmic id] sid0 type%x sub%x rev%x | sid2 type%x sub%x rev%x | %s",
             t0, s0, r0, t2, s2, r2, ok ? "PM8998+PMI8998" : "UNEXPECTED: no writes");
    IOLog("fajita-pmic: %s\n", fajita_pmic_rows[1]);
    return ok;
}

void statusRow(unsigned pass) {
    int pon = rd8(0, FPM_PON_RT_STS), vd = rd8(0, FPM_GPIO_BASE(FPM_GPIO_VOL_DOWN) + FPM_GPIO_RT_STS),
        vu = rd8(0, FPM_GPIO_BASE(FPM_GPIO_VOL_UP) + FPM_GPIO_RT_STS);
    unsigned keys = fpm_keys_decode(pon, vd, vu);
    int st1 = rd8(2, FPM_CHG_STATUS_1), st2 = rd8(2, FPM_CHG_STATUS_2), pp = rd8(2, FPM_POWER_PATH_STATUS), icl = rd8(2, FPM_ICL_STATUS);
    int en = rd8(2, FPM_CHG_ENABLE_CMD), susp = rd8(2, FPM_USBIN_CMD_IL), fv = rd8(2, FPM_FLOAT_VOLTAGE_CFG), fcc = rd8(2, FPM_FAST_CHARGE_CFG);
    int iclcfg = rd8(2, FPM_USBIN_CURRENT_LIMIT_CFG), plug = rd8(2, FPM_USB_INT_RT_STS), apsd = rd8(2, FPM_APSD_RESULT);
    uint8_t v[2] = {}, c[2] = {}, s[2] = {}, t[2] = {};
    int rv = rd16(2, FPM_FG_BATT_VOLTAGE, v), rc = rd16(2, FPM_FG_BATT_CURRENT, c), rs = rd16(2, FPM_FG_MSOC, s), rt = rd16(2, FPM_FG_BATT_TEMP, t);
    int online = pp >= 0 && fpm_usb_online((uint8_t)pp);
    snprintf(fajita_pmic_rows[2], sizeof fajita_pmic_rows[2],
             "[pmic] t=%us #%u keys%x(pon%02x vd%02x vu%02x) chg %s st%02x/%02x pp%02x on%d plug%02x apsd%02x en%d susp%d icl%umA/cfg%d fcc%d fv%02x",
             uptime(), pass, keys, pon & 0xff, vd & 0xff, vu & 0xff, st1 >= 0 ? fpm_charger_state_name((uint8_t)st1) : "?", st1 & 0xff,
             st2 & 0xff, pp & 0xff, online, plug & 0xff, apsd & 0xff, en & 1, susp & 1, icl >= 0 ? fpm_icl_ma((uint8_t)icl) : 0,
             iclcfg, fcc, fv & 0xff);
    snprintf(fajita_pmic_rows[3], sizeof fajita_pmic_rows[3], "[pmic fg] t=%us vbat %dmV ibat %dmA (+chg) msoc %02x/%02x=%d%% temp %d.%dC raw v%02x%02x i%02x%02x%s",
             uptime(), rv ? -1 : fpm_fg_voltage_mv(v[0], v[1]), rc ? 0 : fpm_fg_current_ma(c[0], c[1]), s[0], s[1],
             rs ? -1 : fpm_fg_soc_percent(s[0], s[1]), rt ? 0 : fpm_fg_temp_dc(t[0], t[1]) / 10,
             rt ? 0 : (fpm_fg_temp_dc(t[0], t[1]) % 10 + 10) % 10, v[1], v[0], c[1], c[0], (rv || rc || rs) ? " ERR" : "");
    IOLog("fajita-pmic: %s\nfajita-pmic: %s\n", fajita_pmic_rows[2], fajita_pmic_rows[3]);
    fajita_pmic_state[4] = pass;
}

void charge() {
    fpm_write seq[FPM_CHARGE_SEQ_LEN + 1];
    unsigned n = fpm_charge_sequence(seq, FPM_CHARGE_SEQ_LEN + 1, gFcc, gIcl);
    if (!n || !fpm_sequence_is_safe(seq, n)) {IOLog("fajita-pmic: charge sequence refused (unsafe)\n"); return;}
    unsigned done = 0, errors = 0;
    for (unsigned i = 0; i < n; ++i) {
        const fpm_write &w = seq[i];
        int old = rd8(2, w.addr);
        if (old < 0) {IOLog("fajita-pmic: chg %04x read %d: stop\n", w.addr, old); ++errors; break;}
        uint8_t want = fpm_apply((uint8_t)old, w.mask, w.value);
        int rc = 0, back = old;
        if (want != (uint8_t)old || w.addr == FPM_AICL_RERUN_TIME_CFG) {
            rc = native_spmi_masked_write(2, w.addr, w.mask, w.value);
            back = rd8(2, w.addr);
            ++done;
        }
        IOLog("fajita-pmic: chg %04x %02x>%02x m%02x rb%02x rc%d %s\n", w.addr, old, want, w.mask, back & 0xff, rc, w.what);
        if (rc || back != want) {++errors; IOLog("fajita-pmic: chg write mismatch: stop\n"); break;}
    }
    fajita_pmic_state[2] = done;
    fajita_pmic_state[3] = errors;
    snprintf(fajita_pmic_rows[4], sizeof fajita_pmic_rows[4], "[pmic chg] seq %u writes %u errors %u fcc %u icl %u", n, done, errors,
             seq[15].value * 25u, seq[n - 1].value * 25u);
    IOLog("fajita-pmic: %s\n", fajita_pmic_rows[4]);
}

void publishKeys(unsigned mask) {
    if (mask == native_button_state) return;
    native_button_state = mask;
    fajita_pmic_keys[2] = fajita_pmic_keys[2] + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    void (*sink)(void *) = gButtonSink;
    if (sink) {sink(gButtonSinkContext); fajita_pmic_keys[5] = fajita_pmic_keys[5] + 1;}
}

void keysRow(const char *why) {
    snprintf(fajita_pmic_rows[5], sizeof fajita_pmic_rows[5], "[pmic keys] t=%us %s polls%u err%u chg%u real%x synth%u sink%u state%x | hid sent%u res%x last%02x sink%u",
             uptime(), why, fajita_pmic_keys[0], fajita_pmic_keys[1], fajita_pmic_keys[2], fajita_pmic_keys[3], fajita_pmic_keys[4],
             fajita_pmic_keys[5], native_button_state, &native_hid_button_stats ? native_hid_button_stats[0] : 0,
             &native_hid_button_stats ? native_hid_button_stats[1] : 0, &native_hid_button_stats ? native_hid_button_stats[2] : 0,
             &native_hid_button_stats ? native_hid_button_stats[3] : 0);
    IOLog("fajita-pmic: %s\n", fajita_pmic_rows[5]);
}

// The key poll: 20 ms, two-sample debounce, plus the scripted synthetic presses (unattended test).
void keysThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fajita-pmic-keys");
    if (native_spmi_init()) {keysRow("no spmi"); return;}
    fpm_debounce d = {};
    unsigned synthBits = 0, round = 0, lastRow = 0;
    uint64_t synthNext = gSynth ? (uint64_t)gSynth * 1000 : 0, step = 0;
    struct {unsigned key, at, len;} script[] = {{FPM_KEY_VOL_UP, 0, 200}, {FPM_KEY_VOL_DOWN, 3000, 200}, {FPM_KEY_POWER, 6000, 200}};
    for (;;) {
        int pon = rd8(0, FPM_PON_RT_STS), vd = rd8(0, FPM_GPIO_BASE(FPM_GPIO_VOL_DOWN) + FPM_GPIO_RT_STS),
            vu = rd8(0, FPM_GPIO_BASE(FPM_GPIO_VOL_UP) + FPM_GPIO_RT_STS);
        fajita_pmic_keys[0] = fajita_pmic_keys[0] + 1;
        if (pon < 0 || vd < 0 || vu < 0) fajita_pmic_keys[1] = fajita_pmic_keys[1] + 1;
        unsigned real = fpm_debounce_step(&d, fpm_keys_decode(pon, vd, vu), 2);
        if (real != fajita_pmic_keys[3]) {fajita_pmic_keys[3] = real; keysRow("real change");}
        if (synthNext && round < gSynthCount) {
            uint64_t now = ticks() * 1000 / tickHz();
            if (now >= synthNext) {
                uint64_t t = now - synthNext;
                synthBits = 0;
                for (auto &e : script)
                    if ((e.key & gSynthMask) && t >= e.at && t < e.at + e.len) synthBits |= e.key;
                if (synthBits & ~step) fajita_pmic_keys[4] = fajita_pmic_keys[4] + 1;   // a synthetic press begins
                step = synthBits;
                if (t >= 6000 + 200 + 1000) {++round; synthNext = now + 20000; keysRow("synth round done");}
            }
        }
        unsigned before = native_button_state;
        publishKeys(real | synthBits);
        if (before != native_button_state) keysRow(synthBits ? "synth" : "state");
        if (uptime() - lastRow >= 30) {lastRow = uptime(); keysRow("periodic");}
        IOSleep(20);
    }
}

}   // namespace

// The battery as powerd finds it: an IOPMPowerSource with CurrentCapacity / MaxCapacity (percent of 100), charging and charger state.
class AppleSmartBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(AppleSmartBattery)
public:
    void number(const char *key, long long value) {
        const OSSymbol *k = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
        if (k && n) setPSProperty(k, n);
        OSSafeReleaseNULL(n); OSSafeReleaseNULL(k);
    }
    void flag(const char *key, bool value) {
        const OSSymbol *k = OSSymbol::withCString(key);
        if (k) setPSProperty(k, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(k);
    }
    void report(int percent, int mv, int ma, int temp_dc, bool external, int charging) {
        setBatteryInstalled(true);
        setExternalConnected(external);
        setExternalChargeCapable(external);
        setIsCharging(charging == 1);
        flag("FullyCharged", external && (charging == 2 || percent >= 100));
        flag("AppleRawExternalConnected", external);
        setCurrentCapacity((unsigned)percent);
        setMaxCapacity(100);
        number("AppleRawCurrentCapacity", 3700LL * percent / 100);
        number("AppleRawMaxCapacity", 3700);
        number("NominalChargeCapacity", 3700);
        number("DesignCapacity", 3700);
        setVoltage((unsigned)mv);
        setAmperage(ma);
        number("InstantAmperage", ma);
        number("Temperature", temp_dc * 10);   // hundredths of a degree
        setCycleCount(1);
        OSDictionary *adapter = OSDictionary::withCapacity(3);
        if (adapter) {
            OSNumber *family = OSNumber::withNumber(external ? 0xe0004000ULL : 0ULL, 32);   // USB host port / disconnected
            OSNumber *watts = OSNumber::withNumber(external ? 7ULL : 0ULL, 32);
            if (family) adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family);
            if (watts && external) adapter->setObject(kIOPMPSAdapterDetailsWattsKey, watts);
            const OSSymbol *k = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
            if (k) setPSProperty(k, adapter);
            OSSafeReleaseNULL(k); OSSafeReleaseNULL(family); OSSafeReleaseNULL(watts); OSSafeReleaseNULL(adapter);
        }
        updateStatus();
    }
};
OSDefineMetaClassAndStructors(AppleSmartBattery, IOPMPowerSource)

namespace {
AppleSmartBattery *gBattery;

void batteryReport() {
    uint8_t v[2] = {}, c[2] = {}, s[2] = {}, t[2] = {};
    if (rd16(2, FPM_FG_BATT_VOLTAGE, v) || rd16(2, FPM_FG_BATT_CURRENT, c) || rd16(2, FPM_FG_MSOC, s) || rd16(2, FPM_FG_BATT_TEMP, t)) return;
    int st1 = rd8(2, FPM_CHG_STATUS_1), st2 = rd8(2, FPM_CHG_STATUS_2), pp = rd8(2, FPM_POWER_PATH_STATUS);
    if (st1 < 0 || st2 < 0 || pp < 0) return;
    int online = fpm_usb_online((uint8_t)pp);
    int charging = fpm_charging((uint8_t)st1, (uint8_t)st2, online);
    int percent = fpm_fg_soc_percent(s[0], s[1]);
    int mv = fpm_fg_voltage_mv(v[0], v[1]), ma = fpm_fg_current_ma(c[0], c[1]), temp = fpm_fg_temp_dc(t[0], t[1]);
    if (!gBattery) {
        AppleSmartBattery *b = OSTypeAlloc(AppleSmartBattery);
        IOService *platform = IOService::getPlatform();
        if (!b || !platform || !b->init()) {OSSafeReleaseNULL(b); return;}
        b->report(percent, mv, ma, temp, online, charging);
        if (!b->attach(platform)) {b->release(); return;}
        if (!b->start(platform)) {b->detach(platform); b->release(); return;}
        b->registerService();
        gBattery = b;
        IOLog("fajita-pmic: battery power source up: %d%% %dmV %dmA %s\n", percent, mv, ma,
              online ? (charging == 1 ? "charging" : "on charger") : "on battery");
    } else {
        gBattery->report(percent, mv, ma, temp, online, charging);
    }
    snprintf(fajita_pmic_rows[6], sizeof fajita_pmic_rows[6], "[pmic batt] t=%us %d%% %dmV %dmA ext%d chg%d up%d", uptime(), percent, mv,
             ma, online, charging, gBattery != nullptr);
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fajita-pmic");
    unsigned wdog = 0;
    if (PE_parse_boot_argn("fajita-wdog", &wdog, sizeof wdog) && wdog && &armcpu_wdog)
        for (unsigned s = 0; s < 120 && !armcpu_wdog[0]; ++s) IOSleep(1000);
    int rc = native_spmi_init();
    snprintf(fajita_pmic_rows[0], sizeof fajita_pmic_rows[0], "[pmic] stage %u spmi %d | %s", gStage, rc, native_spmi_row);
    IOLog("fajita-pmic: %s\n", fajita_pmic_rows[0]);
    if (rc) return;
    fajita_pmic_state[0] = 1;
    logApids();
    bool ok = identity();
    fajita_pmic_state[1] = ok;
    if (gDump) dumpAll();
    statusRow(0);
    if (gBatt) batteryReport();
    if (gStage >= 2 && ok) {
        charge();
        fajita_pmic_state[0] = 2;
        IOSleep(2000);
        statusRow(1);
    }
    for (unsigned pass = 2;; ++pass) {
        IOSleep(gPeriod * 1000);
        statusRow(pass);
        if (gBatt) {batteryReport(); if (pass % 6 == 0) IOLog("fajita-pmic: %s\n", fajita_pmic_rows[6]);}
        if (pass % 6 == 0) IOLog("fajita-pmic: %s\n", native_spmi_row);
    }
}
}

// Called by the boot progress monitor once a second (BootProgress.c); the first call after IOKit is up starts the worker.
extern "C" void native_button_set_sink(void (*sink)(void *), void *context) {
    gButtonSink = nullptr;
    __asm__ volatile("dmb ish" ::: "memory");
    gButtonSinkContext = context;
    __asm__ volatile("dmb ish" ::: "memory");
    gButtonSink = sink;
}

extern "C" void fajita_pmic_poll(void) {
    static bool absent;
    if (absent || gStarted) return;
    if (!IOService::getPlatform()) return;
    unsigned stage = 0;
    PE_parse_boot_argn("fajita-pmic", &stage, sizeof stage);
    PE_parse_boot_argn("fajita-pmic-keys", &gKeys, sizeof gKeys);
    if (!stage && !gKeys) {absent = true; return;}
    PE_parse_boot_argn("fajita-pmic-keys-synth", &gSynth, sizeof gSynth);
    PE_parse_boot_argn("fajita-pmic-keys-synth-count", &gSynthCount, sizeof gSynthCount);
    PE_parse_boot_argn("fajita-pmic-keys-synth-mask", &gSynthMask, sizeof gSynthMask);
    PE_parse_boot_argn("fajita-pmic-batt", &gBatt, sizeof gBatt);
    PE_parse_boot_argn("fajita-pmic-period", &gPeriod, sizeof gPeriod);
    PE_parse_boot_argn("fajita-pmic-fcc", &gFcc, sizeof gFcc);
    PE_parse_boot_argn("fajita-pmic-icl", &gIcl, sizeof gIcl);
    PE_parse_boot_argn("fajita-pmic-dump", &gDump, sizeof gDump);
    if (!gPeriod) gPeriod = 10;
    gStage = stage;
    gStarted = true;
    thread_t thread = nullptr;
    if (stage && kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    thread = nullptr;
    if (gKeys && kernel_thread_start_priority(keysThread, nullptr, 80, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
