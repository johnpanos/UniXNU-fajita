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
// `fajita-pmic-bq=1` (with a stage): the bq27411 gauge on i2c10, read only: GCC votes and branch states of QUP wrapper 1 SE2, its
//   DFS levels and pins 55/56, and the engine only if the firmware left its clocks on. `=2`: also vote the SE2 clocks (shared vote,
//   never removed), put 55/56 on qup10, bring the engine up at 100 kHz and read the gauge (standard commands only) every period.
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
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#include "NativeSpmi.h"
#include "FajitaGaugeI2c.h"
#include "FajitaPmicBoard.h"

extern "C" {
char fajita_pmic_rows[8][160];
volatile uint32_t fajita_pmic_state[8];   // [0] stage reached, [1] identity ok, [2] writes done, [3] write errors, [4] rows
extern volatile uint32_t armcpu_wdog[6] __attribute__((weak_import));
volatile uint32_t native_button_state;        // bit 0 power, 1 volume up, 2 volume down (debounced real keys | synthetic)
volatile uint32_t fajita_pmic_keys[8];        // [0] polls, [1] read errors, [2] changes, [3] real mask, [4] synthetic presses, [5] sink calls
extern volatile uint32_t native_hid_button_stats[4] __attribute__((weak_import));
kern_return_t kernel_thread_start_priority(thread_continue_t, void *, integer_t, thread_t *);
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
}

namespace {
unsigned gStage, gPeriod = 10, gFcc, gIcl, gDump = 1, gKeys, gSynth, gSynthCount = 1, gSynthMask = 7, gBatt, gBq;
bool gStarted;
void (*gButtonSink)(void *);
void *gButtonSinkContext;

// Milliseconds since boot (mach_absolute_time, the kernel log's clock; CNTPCT keeps counting across a warm reset).
uint64_t uptimeMs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
unsigned uptime() {return (unsigned)(uptimeMs() / 1000);}

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

unsigned gExtraBits;   // bit 3: the alert slider's ringer state (FajitaAlertSlider.cpp, native-hid-ringer=1)

void publishKeys(unsigned mask) {
    mask = (mask & 7) | gExtraBits;
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

// `key power|up|down [MS]` over the USB debug channel (kcmd): one synthetic press (default 200 ms) through the same publication as the
// real keys, for unattended runs (waking the lock screen, the volume HUD). The key poll applies it.
volatile unsigned gManualBits;
volatile uint64_t gManualUntil;
extern "C" int fajita_usbdbg_register_kcmd(const char *name, size_t (*fn)(const char *, size_t, char *, size_t), const char *help)
    __attribute__((weak));
size_t kcmdKey(const char *arg, size_t len, char *out, size_t cap) {
    char a[48] = {};
    memcpy(a, arg, len < sizeof a - 1 ? len : sizeof a - 1);
    unsigned key = !strncmp(a, "power", 5) ? FPM_KEY_POWER : !strncmp(a, "up", 2) ? FPM_KEY_VOL_UP
                 : !strncmp(a, "down", 4) ? FPM_KEY_VOL_DOWN : 0;
    if (!key) return (size_t)snprintf(out, cap, "usage: key power|up|down [MS]\n");
    const char *sp = strchr(a, ' ');
    unsigned ms = 200;
    if (sp) { ms = 0; for (++sp; *sp >= '0' && *sp <= '9'; ++sp) ms = ms * 10 + (unsigned)(*sp - '0'); }
    if (ms < 40) ms = 40;
    if (ms > 5000) ms = 5000;
    gManualUntil = uptimeMs() + ms;
    gManualBits = key;
    return (size_t)snprintf(out, cap, "key %s pressed for %u ms\n", a, ms);
}

// The key poll: 20 ms, two-sample debounce, plus the scripted synthetic presses (unattended test).
void keysThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "fajita-pmic-keys");
    if (native_spmi_init()) {keysRow("no spmi"); return;}
    if (fajita_usbdbg_register_kcmd) fajita_usbdbg_register_kcmd("key", kcmdKey, "synthetic key press: key power|up|down [MS]");
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
            uint64_t now = uptimeMs();
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
        unsigned manual = 0;
        if (gManualBits) { if (uptimeMs() < gManualUntil) manual = gManualBits; else gManualBits = 0; }
        unsigned before = native_button_state;
        publishKeys(real | synthBits | manual);
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

bool gaugeLatest(fpm_bq &out);

void batteryReport() {
    uint8_t v[2] = {}, c[2] = {}, s[2] = {}, t[2] = {};
    if (rd16(2, FPM_FG_BATT_VOLTAGE, v) || rd16(2, FPM_FG_BATT_CURRENT, c) || rd16(2, FPM_FG_MSOC, s) || rd16(2, FPM_FG_BATT_TEMP, t)) return;
    int st1 = rd8(2, FPM_CHG_STATUS_1), st2 = rd8(2, FPM_CHG_STATUS_2), pp = rd8(2, FPM_POWER_PATH_STATUS);
    if (st1 < 0 || st2 < 0 || pp < 0) return;
    int online = fpm_usb_online((uint8_t)pp);
    int charging = fpm_charging((uint8_t)st1, (uint8_t)st2, online);
    int percent = fpm_fg_soc_percent(s[0], s[1]);
    int mv = fpm_fg_voltage_mv(v[0], v[1]), ma = fpm_fg_current_ma(c[0], c[1]), temp = fpm_fg_temp_dc(t[0], t[1]);
    // The bq27411 is the battery's own gauge: its percentage, voltage, average current and temperature when it answers sensibly.
    fpm_bq bq;
    bool fromBq = gaugeLatest(bq);
    if (fromBq) {percent = bq.soc; mv = bq.mv; ma = bq.ma; temp = bq.temp_dc;}
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
    snprintf(fajita_pmic_rows[6], sizeof fajita_pmic_rows[6], "[pmic batt] t=%us %d%% %dmV %dmA ext%d chg%d up%d src %s", uptime(), percent, mv,
             ma, online, charging, gBattery != nullptr, fromBq ? "bq27411" : "pmi8998-fg");
}

// ---- the bq27411 on i2c10 ----
struct Gauge {
    uintptr_t gccBranch, gccVote, wrapper, se, pin[2];
    bool up;
    int level;
    geni_i2c i2c;
    fpm_bq last;
    unsigned reads, errors;
    int lastErr;
} gGauge;

uintptr_t mapIo(uint64_t phys, uint64_t size) {return (uintptr_t)ml_io_map((vm_offset_t)phys, (vm_size_t)size);}
uint32_t mrd(uintptr_t base, uint32_t off) {return *reinterpret_cast<volatile uint32_t *>(base + off);}
void mwr(uintptr_t base, uint32_t off, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(base + off) = v; __asm__ volatile("dsb sy" ::: "memory");}
uint32_t gIoRd(void *ctx, uint32_t reg) {return mrd(reinterpret_cast<uintptr_t>(ctx), reg);}
void gIoWr(void *ctx, uint32_t reg, uint32_t value) {mwr(reinterpret_cast<uintptr_t>(ctx), reg, value);}
void gIoDelay(void *, unsigned us) {
    if (us > 50) {IODelay(us); return;}
    uint64_t f = 0, t0 = 0, t = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(t0));
    uint64_t n = (f ? f : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(t));} while (t - t0 < n);
}

bool gaugeClocksOn(const Gauge &g) {
    return fpm_branch_running(mrd(g.gccBranch, FPM_HALT_M_AHB & 0xfff)) && fpm_branch_running(mrd(g.gccBranch, FPM_HALT_S_AHB & 0xfff)) &&
           !(mrd(g.gccBranch, FPM_HALT_S2 & 0xfff) & FPM_CBCR_CLK_OFF);
}

// Read only: votes, branches, DFS, pins; the engine only with its clocks already running.
void gaugeSnapshot(Gauge &g) {
    g.gccBranch = mapIo(FPM_GCC_BASE + 0x18000, 0x1000);
    g.gccVote = mapIo(FPM_GCC_BASE + 0x52000, 0x1000);
    g.pin[0] = mapIo(FPM_TLMM_BASE + fpm_pin_offset(FPM_GPIO_BQ_SDA), 0x1000);
    g.pin[1] = mapIo(FPM_TLMM_BASE + fpm_pin_offset(FPM_GPIO_BQ_SCL), 0x1000);
    if (!g.gccBranch || !g.gccVote || !g.pin[0] || !g.pin[1]) {IOLog("fajita-pmic: bq map failed\n"); return;}
    uint32_t cfg[FPM_DFS_LEVELS], m[FPM_DFS_LEVELS], n[FPM_DFS_LEVELS];
    for (unsigned l = 0; l < FPM_DFS_LEVELS; ++l) {
        cfg[l] = mrd(g.gccBranch, (FPM_S2_RCG + FPM_RCG_PERF_CFG(l)) & 0xfff);
        m[l] = mrd(g.gccBranch, (FPM_S2_RCG + FPM_RCG_PERF_M(l)) & 0xfff);
        n[l] = mrd(g.gccBranch, (FPM_S2_RCG + FPM_RCG_PERF_N(l)) & 0xfff);
    }
    uint32_t dfsr = mrd(g.gccBranch, (FPM_S2_RCG + FPM_RCG_DFSR) & 0xfff), rcfg = mrd(g.gccBranch, (FPM_S2_RCG + FPM_RCG_CFG) & 0xfff);
    g.level = fpm_xo_level(dfsr, cfg, m, n, FPM_DFS_LEVELS, rcfg);
    IOLog("fajita-pmic: bq clk vote%08x m%08x s%08x s2%08x dfs%x cfg%08x xoL%d pin55 %03x/%x pin56 %03x/%x\n", mrd(g.gccVote, FPM_GCC_VOTE & 0xfff),
          mrd(g.gccBranch, FPM_HALT_M_AHB & 0xfff), mrd(g.gccBranch, FPM_HALT_S_AHB & 0xfff), mrd(g.gccBranch, FPM_HALT_S2 & 0xfff), dfsr,
          rcfg, g.level, mrd(g.pin[0], 0) & 0x3ff, mrd(g.pin[0], 4) & 3, mrd(g.pin[1], 0) & 0x3ff, mrd(g.pin[1], 4) & 3);
    if (!gaugeClocksOn(g)) {IOLog("fajita-pmic: bq se not read: clocks off\n"); return;}
    g.se = mapIo(FPM_BQ_SE_BASE, FPM_BQ_SE_SIZE);
    if (!g.se) return;
    IOLog("fajita-pmic: bq se fw%08x ifdis%x hw0%08x st%08x clk%x/%x\n", mrd(g.se, GI2C_FW_REVISION_RO), mrd(g.se, GI2C_IF_DISABLE_RO),
          mrd(g.se, GI2C_HW_PARAM_0), mrd(g.se, GI2C_GENI_STATUS), mrd(g.se, GI2C_CLK_SEL), mrd(g.se, GI2C_SER_M_CLK_CFG));
}

bool gaugeEnable(Gauge &g) {
    if (!g.gccVote || g.level < 0) {IOLog("fajita-pmic: bq enable refused (no map or no 19.2 MHz level)\n"); return false;}
    const uint32_t want = FPM_VOTE_M_AHB | FPM_VOTE_S_AHB | FPM_VOTE_S2;
    mwr(g.gccVote, FPM_GCC_VOTE & 0xfff, mrd(g.gccVote, FPM_GCC_VOTE & 0xfff) | want);   // shared vote, never removed
    unsigned us = 0;
    while (!gaugeClocksOn(g) && us < 20000) {IODelay(10); us += 10;}
    if (!gaugeClocksOn(g)) {IOLog("fajita-pmic: bq clocks stayed off after %uus\n", us); return false;}
    // qup_i2c10_default: function qup10; 2 mA, no bias, as the i2c pinconf of the board (the gauge's pull-ups are on the board).
    mwr(g.pin[0], 0, fpm_pin_cfg(FPM_FUNC_QUP10, 2, 0));
    mwr(g.pin[1], 0, fpm_pin_cfg(FPM_FUNC_QUP10, 2, 0));
    if (!g.se) g.se = mapIo(FPM_BQ_SE_BASE, FPM_BQ_SE_SIZE);
    g.wrapper = mapIo(FPM_QUP1_WRAPPER_BASE, 0x1000);
    if (!g.se || !g.wrapper) return false;
    g.i2c.se = geni_i2c_io{reinterpret_cast<void *>(g.se), gIoRd, gIoWr, gIoDelay};
    g.i2c.wrapper = geni_i2c_io{reinterpret_cast<void *>(g.wrapper), gIoRd, gIoWr, gIoDelay};
    g.i2c.poll_limit = 20000;
    int rc = geni_i2c_init(&g.i2c);
    if (!rc) rc = geni_i2c_set_clock(&g.i2c, (unsigned)g.level, FPM_TCXO_HZ, FPM_BQ_HZ);
    IOLog("fajita-pmic: bq engine %uus fw%04x depth%u init/clock %d\n", us, g.i2c.info.fw_revision & 0xffff, g.i2c.info.tx_fifo_depth, rc);
    g.up = rc == 0;
    return g.up;
}

int gaugeWord(Gauge &g, uint8_t reg, uint16_t &out) {
    uint8_t b[2] = {};
    int rc = geni_i2c_write_read(&g.i2c, FPM_BQ_ADDR, &reg, 1, b, 2);
    if (rc && rc != GI2C_ERR_ADDR_NACK) geni_i2c_recover(&g.i2c);
    out = (uint16_t)(b[0] | b[1] << 8);
    return rc;
}

void gaugeRead(Gauge &g) {
    if (!g.up) return;
    static const uint8_t regs[8] = {FPM_BQ_TEMPERATURE, FPM_BQ_VOLTAGE, FPM_BQ_FLAGS, FPM_BQ_REMAINING, FPM_BQ_FULL, FPM_BQ_AVG_CURRENT,
                                    FPM_BQ_SOC, FPM_BQ_SOH};
    uint16_t w[8] = {};
    for (unsigned i = 0; i < 8; ++i) {
        int rc = gaugeWord(g, regs[i], w[i]);
        if (rc) {++g.errors; g.lastErr = rc; IOLog("fajita-pmic: bq read %02x err %d\n", regs[i], rc); return;}
    }
    ++g.reads;
    fpm_bq_decode(&g.last, w);
    snprintf(fajita_pmic_rows[7], sizeof fajita_pmic_rows[7], "[pmic bq] t=%us %dmV %dmA soc%d%% %d/%dmAh soh%d%% %d.%dC flags%04x%s err%u",
             uptime(), g.last.mv, g.last.ma, g.last.soc, g.last.remaining_mah, g.last.full_mah, g.last.soh, g.last.temp_dc / 10,
             (g.last.temp_dc % 10 + 10) % 10, g.last.flags, g.last.valid ? "" : " IMPLAUSIBLE", g.errors);
    IOLog("fajita-pmic: %s\n", fajita_pmic_rows[7]);
}

bool gaugeLatest(fpm_bq &out) {
    if (!gGauge.up || !gGauge.last.valid || !gGauge.reads) return false;
    out = gGauge.last;
    return true;
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
    if (gBq) {gaugeSnapshot(gGauge); if (gBq >= 2 && gaugeEnable(gGauge)) gaugeRead(gGauge);}
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
        if (gBq >= 2) gaugeRead(gGauge);
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
    PE_parse_boot_argn("fajita-pmic-bq", &gBq, sizeof gBq);
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

// The alert slider (FajitaAlertSlider.cpp): bit 3 of native_button_state, which NativeHidInput reports as Telephony 0x2E (the ringer
// switch usage of AppleM68Buttons) when native-hid-ringer=1.
extern "C" void fajita_button_extra(unsigned bits) {
    gExtraBits = bits & 8;
    publishKeys(native_button_state & 7);
}
