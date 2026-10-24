// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita panel brightness: DCS 0x51 (Write Display Brightness) to the OnePlus 6T's Samsung AMOLED through the SDM845 DSI0
// command DMA. Reference: mainline drivers/gpu/drm/panel/panel-samsung-s6e3fc2x01.c (s6e3fc2x01_panel_bl_update_status: LPM
// cleared for the write, mipi_dsi_dcs_set_display_brightness_large = 2 payload bytes, big endian, 0..1023) and
// drivers/gpu/drm/msm/dsi/dsi_host.c (msm_dsi_host_xfer_prepare / dsi_cmd_dma_add / msm_dsi_host_cmd_xfer_commit).
//
// Boot argument fajita-brightness=<n> (default 0 = nothing happens: no kcmd, no service, no register access):
//   1  registers `kcmd brightness` and publishes the "lcd" platform-function service that the iOS AppleARMBacklight calls
//   2  as 1, and reads DSI/SMMU state at boot into the kernel log (read only)
// kcmd brightness N (0..1023 panel DBV) | read | regs | raw
//
// iOS side: the iPhone X device tree has /backlight (class AppleARMBacklight, IOPlatformDevice, IONameMatch "backlight") whose
// function-backlight_update / function-backlight_enable are { phandle 0x80, fourcc 'Uklb' / 'Elkb' }: AppleARMBacklight calls
// the service holding phandle 0x80 (the panel node /arm-io/mipi-dsim/lcd) with platform functions "bklU" (new level) and "bklE"
// (enable). FajitaLcd is that service. CoreBrightness (backboardd) drives AppleARMBacklight; the slider ends here.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
char fajita_panel_row[256];
volatile uint32_t fajita_panel_state[8];   // [0] arg, [1] sent, [2] last level, [3] last error, [4] last INTR, [5] last readback, [6] calls from iOS, [7] last iOS value
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
}

namespace {
enum : uint32_t {
    kDsiBase = 0xae94000, kDsiSize = 0x400,
    // The tail of the 36 MiB splash carve-out at 0x9d400000 (the 1080x2340 scanout uses the first ~10 MiB).
    kBufPhys = 0x9d400000 + 0x1800000, kBufSize = 0x1000,
    kCtrl = 0x000, kStatus0 = 0x004, kCmdDmaCtrl = 0x038, kDmaBase = 0x044, kDmaLen = 0x048, kAckErr = 0x064, kRdbk0 = 0x068,
    kTrigCtrl = 0x080, kTrigDma = 0x08c, kIntrCtrl = 0x10c, kClkCtrl = 0x118,
    kCtrlEnable = 1u << 0, kCtrlCmdMode = 1u << 2,
    kDmaLowPower = 1u << 26,
    kIntrDmaDone = 1u << 0, kIntrDmaMask = 1u << 1,
    kMaxLevel = 1023,
};
OSSharedPtr<IOMemoryMap> gDsi, gBuf;
IOLock *gLock;
bool gEnabled;
unsigned gMode;

struct Out {
    char *p; size_t cap, n;
    Out(char *b, size_t c) : p(b), cap(c), n(0) {if (c) p[0] = 0;}
    void add(const char *text) {add("%s", text);}
    template <typename A0, typename... A> void add(const char *fmt, A0 a0, A... a) {
        if (n + 1 >= cap) return;
        int w = snprintf(p + n, cap - n, fmt, a0, a...);
        if (w > 0) n = n + size_t(w) >= cap ? cap - 1 : n + size_t(w);
    }
};

volatile uint32_t &dsi(uint32_t off) {return *reinterpret_cast<volatile uint32_t *>(gDsi->getVirtualAddress() + off);}
volatile uint8_t *buf() {return reinterpret_cast<volatile uint8_t *>(gBuf->getVirtualAddress());}

bool map(Out &o) {
    if (!gDsi) {
        IODeviceMemory *m = IODeviceMemory::withRange(kDsiBase, kDsiSize);
        if (m) {gDsi = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain); m->release();}
    }
    if (!gBuf) {
        IODeviceMemory *m = IODeviceMemory::withRange(kBufPhys, kBufSize);
        if (m) {gBuf = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain); m->release();}
    }
    if (!gDsi || !gBuf) {o.add("map dsi=%d buf=%d", gDsi ? 1 : 0, gBuf ? 1 : 0); return false;}
    return true;
}

// One packet through the command DMA, the way msm_dsi_host_xfer_prepare/cmd_tx/xfer_restore do: CTRL |= ENABLE|CMD_MODE_EN,
// high-speed (LOW_POWER clear) unless lp, CMD_DMA_DONE unmasked, DMA_BASE/DMA_LEN/TRIG_DMA, wait for CMD_DMA_DONE, restore.
// The packet is in the MSM memory format: { wc lo (or data0), wc hi (or data1), data type, flags } + payload, padded to 4 with 0xff.
// Returns 0 when the controller reported CMD_DMA_DONE, else a negative error. *intrOut = INTR_CTRL at the end.
int dmaSend(const uint8_t *pkt, uint32_t len, bool lp, uint32_t *intrOut) {
    const uint32_t ctrl = dsi(kCtrl), dma = dsi(kCmdDmaCtrl), intr0 = dsi(kIntrCtrl);
    uint32_t padded = (len + 3) & ~3u;
    if (padded > kBufSize) return -22;
    for (uint32_t i = 0; i < padded; ++i) buf()[i] = i < len ? pkt[i] : 0xff;
    __asm__ volatile("dsb sy" ::: "memory");
    dsi(kCtrl) = ctrl | kCtrlEnable | kCtrlCmdMode;
    dsi(kCmdDmaCtrl) = lp ? (dma | kDmaLowPower) : (dma & ~kDmaLowPower);
    dsi(kIntrCtrl) = (intr0 & ~kIntrDmaDone) | kIntrDmaDone | kIntrDmaMask;   // write-1-to-clear the status, unmask the done interrupt
    dsi(kDmaBase) = kBufPhys;
    dsi(kDmaLen) = padded;
    __asm__ volatile("dsb sy" ::: "memory");
    dsi(kTrigDma) = 1;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t v = 0;
    int ret = -110;
    for (unsigned i = 0; i < 20000; ++i) {   // 200 ms like dsi_cmd_dma_tx
        v = dsi(kIntrCtrl);
        if (v & kIntrDmaDone) {ret = 0; break;}
        IODelay(10);
    }
    dsi(kIntrCtrl) = kIntrDmaDone;           // clear done, mask off
    dsi(kIntrCtrl) = intr0 & ~(kIntrDmaDone | kIntrDmaMask);
    dsi(kCmdDmaCtrl) = dma;
    dsi(kCtrl) = ctrl;
    if (intrOut) *intrOut = v;
    return ret;
}

// DCS 0x51 with a 16-bit big-endian value: a DCS long write (0x39), word count 3.
int writeBrightness(unsigned level, uint32_t *intr) {
    const uint8_t pkt[] = {3, 0, 0x39, 0xc0, 0x51, uint8_t(level >> 8), uint8_t(level)};
    return dmaSend(pkt, sizeof pkt, false, intr);
}

// DCS 0x52: SET_MAXIMUM_RETURN_PACKET_SIZE 2 (0x37), then the DCS read (0x06) with the response flag; the reply is in RDBK_DATA(0).
int readBrightness(unsigned *level, uint32_t *intr) {
    const uint8_t maxret[] = {2, 0, 0x37, 0x80};
    const uint8_t rd[] = {0x52, 0, 0x06, 0xa0};
    int r = dmaSend(maxret, sizeof maxret, false, intr);
    if (r) return r;
    r = dmaSend(rd, sizeof rd, false, intr);
    if (r) return r;
    uint32_t raw = dsi(kRdbk0);
    uint32_t be = __builtin_bswap32(raw);    // ntohl: byte 0 is the first byte of the response packet
    uint8_t type = uint8_t(be >> 24), d0 = uint8_t(be >> 16), d1 = uint8_t(be >> 8);
    fajita_panel_state[5] = raw;
    if (type == 0x22) {*level = unsigned(d0) << 8 | d1; return 0;}   // short response, 2 bytes (DT 0x22); 1 byte is 0x21
    if (type == 0x21) {*level = d0; return 0;}
    return -5;
}

int setBrightness(unsigned level, char *msg, size_t cap) {
    Out o(msg, cap);
    if (level > kMaxLevel) level = kMaxLevel;
    if (!gLock) return -19;
    IOLockLock(gLock);
    int r = 0;
    uint32_t intr = 0;
    do {
        if (!map(o)) {r = -12; break;}
        r = writeBrightness(level, &intr);
        fajita_panel_state[4] = intr;
        o.add("dbv %u: %s INTR %08x ACK_ERR %08x STATUS0 %08x", level, r ? "NO DMA DONE" : "dma done", intr, (unsigned)dsi(kAckErr),
              (unsigned)dsi(kStatus0));
    } while (0);
    fajita_panel_state[3] = uint32_t(r);
    if (!r) {fajita_panel_state[1] = fajita_panel_state[1] + 1; fajita_panel_state[2] = level;}
    IOLockUnlock(gLock);
    return r;
}

void dumpRegs(Out &o) {
    static const struct {const char *n; uint32_t off;} regs[] = {{"CTRL", kCtrl}, {"STATUS0", kStatus0}, {"CMD_DMA_CTRL", kCmdDmaCtrl},
        {"CMD_CFG0", 0x3c}, {"CMD_CFG1", 0x40}, {"TRIG_CTRL", kTrigCtrl}, {"EOT", 0xc8}, {"LANE_STATUS", 0xa4}, {"INTR", kIntrCtrl},
        {"CLK_CTRL", kClkCtrl}, {"ACK_ERR", kAckErr}};
    for (const auto &r : regs) o.add("%s=%08x ", r.n, (unsigned)dsi(r.off));
}

size_t kcmdBrightness(const char *args, size_t len, char *out, size_t cap) {
    char a[48] = {};
    for (size_t i = 0; i < len && i < sizeof a - 1; ++i) a[i] = args[i];
    Out o(out, cap);
    if (!gEnabled) {o.add("fajita-brightness=0\n"); return o.n;}
    if (!strncmp(a, "regs", 4)) {
        IOLockLock(gLock);
        if (map(o)) dumpRegs(o);
        IOLockUnlock(gLock);
        o.add("\n");
        return o.n;
    }
    if (!strncmp(a, "read", 4)) {
        IOLockLock(gLock);
        unsigned level = 0; uint32_t intr = 0; int r = -12;
        if (map(o)) {r = readBrightness(&level, &intr); o.add("0x52 -> %d: dbv %u (INTR %08x RDBK %08x) ", r, level, intr, (unsigned)fajita_panel_state[5]);}
        IOLockUnlock(gLock);
        o.add("\n");
        return o.n;
    }
    if (a[0] >= '0' && a[0] <= '9') {
        unsigned level = 0;
        for (const char *p = a; *p >= '0' && *p <= '9'; ++p) level = level * 10 + unsigned(*p - '0');
        char msg[96];
        int r = setBrightness(level, msg, sizeof msg);
        o.add("%s -> %d\n", msg, r);
        return o.n;
    }
    o.add("usage: brightness N (0..%u DBV) | read | regs   state: sent %u last %u err %d iOS calls %u last %u\n", kMaxLevel,
          (unsigned)fajita_panel_state[1], (unsigned)fajita_panel_state[2], (int)fajita_panel_state[3], (unsigned)fajita_panel_state[6],
          (unsigned)fajita_panel_state[7]);
    return o.n;
}

// iOS side. AppleARMBacklight passes the new level through callPlatformFunction("bklU"): the arguments are logged until the
// calling convention is confirmed on the phone (fajita_panel_state[6] counts calls, [7] the last value).
}  // namespace

extern "C" int fajita_panel_set_brightness(unsigned dbv) {
    char msg[96];
    return setBrightness(dbv, msg, sizeof msg);
}

class FajitaLcd : public IOService {
    OSDeclareDefaultStructors(FajitaLcd)
public:
    IOReturn callPlatformFunction(const OSSymbol *name, bool waitForFunction, void *p1, void *p2, void *p3, void *p4) override {
        const char *n = name ? name->getCStringNoCopy() : "";
        fajita_panel_state[6] = fajita_panel_state[6] + 1;
        static unsigned logged;
        if (logged++ < 40)
            IOLog("fajita-panel: callPlatformFunction(\"%s\", wait %d, %p, %p, %p, %p)\n", n, waitForFunction, p1, p2, p3, p4);
        const bool update = !strcmp(n, "bklU") || !strcmp(n, "Uklb");
        const bool enable = !strcmp(n, "bklE") || !strcmp(n, "Elkb");
        if (update) {
            // Level argument: a small integer in p1, or p1 pointing at a 32-bit value. DBV range of the iPhone X backlight node is
            // DBVMin 139 .. DBVMax 0x7ff; the panel takes 0..1023.
            uint64_t v = reinterpret_cast<uint64_t>(p1);
            if (v > 0xffff) return kIOReturnBadArgument;
            fajita_panel_state[7] = uint32_t(v);
            unsigned dbv = unsigned(v * kMaxLevel / 0x7ff);
            if (v && !dbv) dbv = 1;
            return fajita_panel_set_brightness(dbv) ? kIOReturnIOError : kIOReturnSuccess;
        }
        if (enable) return kIOReturnSuccess;
        return IOService::callPlatformFunction(name, waitForFunction, p1, p2, p3, p4);
    }
};
OSDefineMetaClassAndStructors(FajitaLcd, IOService)

// Called once a second by the boot progress monitor (FajitaCpuFreq.cpp's sheng_cpufreq_poll); the first call after IOKit is up does the work.
extern "C" void fajita_panel_poll(void) {
    static bool done;
    if (done || !IOService::getPlatform()) return;
    done = true;
    unsigned mode = 0;
    PE_parse_boot_argn("fajita-brightness", &mode, sizeof mode);
    gMode = mode;
    fajita_panel_state[0] = mode;
    if (!mode) return;
    gEnabled = true;
    gLock = IOLockAlloc();
    if (fajita_usbdbg_register_kcmd) fajita_usbdbg_register_kcmd("brightness", kcmdBrightness, "panel DBV: brightness N (0..1023) | read | regs");
    if (mode >= 2) {
        Out o(fajita_panel_row, sizeof fajita_panel_row);
        IOLockLock(gLock);
        if (map(o)) dumpRegs(o);
        IOLockUnlock(gLock);
        IOLog("fajita-panel: %s\n", fajita_panel_row);
    }
    // The panel node the backlight functions point at: phandle 0x80 (the iPhone X tree's /arm-io/mipi-dsim/lcd).
    FajitaLcd *lcd = OSTypeAlloc(FajitaLcd);
    IOService *platform = reinterpret_cast<IOService *>(IOService::getPlatform());
    OSDictionary *props = OSDictionary::withCapacity(4);
    const uint32_t phandle = 0x80;
    OSData *ph = OSData::withBytes(&phandle, sizeof phandle);
    OSData *nm = OSData::withBytes("lcd", 4);
    if (props && ph && nm) {props->setObject("AAPL,phandle", ph); props->setObject("name", nm); props->setObject("device_type", nm);}
    const bool ok = lcd && props && lcd->init(props) && lcd->attach(platform) && lcd->start(platform);
    if (ph) ph->release();
    if (nm) nm->release();
    if (props) props->release();
    if (!ok) {
        IOLog("fajita-panel: lcd service failed\n");
        return;
    }
    lcd->registerService();
    IOLog("fajita-panel: brightness ready (mode %u), lcd service published\n", mode);
}
