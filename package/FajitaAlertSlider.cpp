// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita alert slider: the OnePlus 6T's three-position switch on TLMM GPIO 126 (top, Silent), 52 (middle, Vibrate) and 24
// (bottom, Ring), active low (sdm845-oneplus-common.dtsi alert-slider: gpio-keys, pinctrl alert_slider_default = function gpio,
// drive 2 mA, bias-disable, i.e. TLMM_GPIO_CFG = 0). TLMM base 0x03400000; pin n at tile + 0x1000*n (pinctrl-sdm845.c: gpio126
// EAST 0x100000, gpio52 NORTH 0x500000, gpio24 SOUTH 0x900000), CFG at +0, IN/OUT at +4 (bit 0 = input level).
//
// fajita-alert=<n> (default 0: nothing is touched): 1 polls once a second (called from sheng_cpufreq_poll), applies the
// bias-disable configuration of that pinctrl state, logs each change, exports fajita_alert_position
// (0 silent, 1 vibrate, 2 ring, 3 none or several asserted) and registers `kcmd alert`. fajita-alert=2 also reports the switch to iOS
// (needs native-hid-buttons=1, native-hid-ringer=1 and the /buttons ringerab entry of make-fajita-adt.py --ringer).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
volatile uint32_t fajita_alert_position = 3;
volatile uint32_t fajita_alert_changes;
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
void fajita_alert_set_sink(void (*)(void *, uint32_t), void *);
void fajita_button_extra(unsigned bits) __attribute__((weak));   // FajitaPmic.cpp
}

namespace {
struct Pin {const char *name; uint32_t pa;};
const Pin kPins[3] = {{"silent", 0x03400000 + 0x100000 + 126 * 0x1000}, {"vibrate", 0x03400000 + 0x500000 + 52 * 0x1000}, {"ring", 0x03400000 + 0x900000 + 24 * 0x1000}};
OSSharedPtr<IOMemoryMap> gMap[3];
unsigned gMode;
void (*gSink)(void *, uint32_t);
void *gSinkCtx;

volatile uint32_t *reg(unsigned i, unsigned off) {return reinterpret_cast<volatile uint32_t *>(gMap[i]->getVirtualAddress() + off);}

bool mapAll() {
    for (unsigned i = 0; i < 3; ++i) {
        if (gMap[i]) continue;
        IODeviceMemory *m = IODeviceMemory::withRange(kPins[i].pa, 0x10);
        if (m) {gMap[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain); m->release();}
        if (!gMap[i]) return false;
    }
    return true;
}

// 0 silent, 1 vibrate, 2 ring, 3 none or several asserted; mask of the asserted (low) pins in *mask.
uint32_t read(unsigned *mask) {
    unsigned m = 0;
    for (unsigned i = 0; i < 3; ++i) if (!(*reg(i, 4) & 1)) m |= 1u << i;
    if (mask) *mask = m;
    return (m == 1) ? 0 : (m == 2) ? 1 : (m == 4) ? 2 : 3;
}

volatile int32_t gSim = -1;   // kcmd alert sim: a position that stands in for the switch until `alert sim off` (-1: the pins)

size_t kcmdAlert(const char *args, size_t len, char *out, size_t cap) {
    if (!mapAll()) return size_t(snprintf(out, cap, "alert: map failed\n"));
    char a[24] = {};
    for (size_t i = 0; i < len && i < sizeof a - 1; ++i) a[i] = args[i];
    if (!strncmp(a, "sim ", 4)) {
        // Unattended tests cannot move the switch: `alert sim silent|vibrate|ring` reports that position to iOS as if the switch had
        // moved there (the next poll), `alert sim off` goes back to the pins.
        const char *w = a + 4;
        int32_t v = !strncmp(w, "silent", 6) ? 0 : !strncmp(w, "vibrate", 7) ? 1 : !strncmp(w, "ring", 4) ? 2 : !strncmp(w, "off", 3) ? -1 : -2;
        if (v == -2) return size_t(snprintf(out, cap, "usage: alert [sim silent|vibrate|ring|off]\n"));
        gSim = v;
        return size_t(snprintf(out, cap, "alert: sim %s (applied at the next poll, <= 1 s)\n", v < 0 ? "off" : w));
    }
    unsigned m = 0;
    uint32_t p = read(&m);
    return size_t(snprintf(out, cap, "alert slider: %s (asserted mask %x: silent=1 vibrate=2 ring=4) cfg %x/%x/%x in %x/%x/%x changes %u%s\n",
        p == 0 ? "SILENT" : p == 1 ? "VIBRATE" : p == 2 ? "RING" : "none/several", m, (unsigned)*reg(0, 0), (unsigned)*reg(1, 0), (unsigned)*reg(2, 0),
        (unsigned)*reg(0, 4) & 1, (unsigned)*reg(1, 4) & 1, (unsigned)*reg(2, 4) & 1, (unsigned)fajita_alert_changes, gSim >= 0 ? " (sim: iOS sees the simulated position)" : ""));
}
}  // namespace

extern "C" void fajita_alert_set_sink(void (*sink)(void *, uint32_t), void *ctx) {gSinkCtx = ctx; gSink = sink;}

extern "C" void fajita_alert_poll(void) {
    static bool done, off;
    if (off) return;
    if (!done) {
        if (!IOService::getPlatform()) return;
        done = true;
        PE_parse_boot_argn("fajita-alert", &gMode, sizeof gMode);
        if (!gMode) {off = true; return;}
        if (!mapAll()) {IOLog("fajita-alert: map failed\n"); off = true; return;}
        for (unsigned i = 0; i < 3; ++i) *reg(i, 0) = 0;   // alert_slider_default: gpio, 2 mA, bias-disable
        if (fajita_usbdbg_register_kcmd) fajita_usbdbg_register_kcmd("alert", kcmdAlert, "alert slider position (GPIO 126/52/24)");
        IOSleep(1);
    }
    unsigned m = 0;
    uint32_t p = read(&m);
    if (gSim >= 0) p = uint32_t(gSim);
    if (p != fajita_alert_position) {
        fajita_alert_position = p;
        fajita_alert_changes = fajita_alert_changes + 1;
        IOLog("fajita-alert: %s (mask %x)\n", p == 0 ? "silent" : p == 1 ? "vibrate" : p == 2 ? "ring" : "none/several", m);
        if (gSink) gSink(gSinkCtx, p);
        // iOS has one ringer switch: Ring = ringer on; Vibrate and Silent = silent (iOS vibrates in silent by its own setting).
        // Bit 3 of native_button_state (Telephony 0x2E, native-hid-ringer=1) is set while silent; fajita-alert-invert=1 flips it.
        if (gMode >= 2 && &fajita_button_extra) {
            unsigned inv = 0;
            PE_parse_boot_argn("fajita-alert-invert", &inv, sizeof inv);
            bool silent = p == 0 || p == 1;
            fajita_button_extra((silent != (inv == 1)) ? 8 : 0);
        }
    }
}
