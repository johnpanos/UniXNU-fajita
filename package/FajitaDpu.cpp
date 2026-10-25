// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita DPU (SDM845 MDSS at 0x0ae00000): inspection of the active scan-out path and a one-shot QSEED3 scale test, the step before the
// display scaler does the logical-to-panel scale (1125x2436 -> 1080x2340) that the GPU presenter's compute pass does today (14 ms of GPU a
// frame). Offsets are Linux's sdm845 catalog (drivers/gpu/drm/msm/disp/dpu1) with the MDP at MDSS + 0x1000: SSPP VIG0 at 0x5000, its
// scaler at +0xa00, CTL0 at 0x2000, LM0 at 0x45000, PP0 at 0x71000, INTF1 at 0x6b800. Only those blocks are touched: the other pipes are
// clock-gated and a read of them stalls the bus (a dump of VIG1-3/DMA hung the phone).
//   kcmd dpu                     VIG0 (size/pitch/format/address), CTL0, LM0, PP0, INTF1, and the last scale test's result
//   kcmd dpu q                   VIG0's QSEED3 and pixel-extension registers
//   kcmd dpu r OFF [N]           N (default 8, max 64) words at MDSS + OFF (keep to the blocks above)
//   kcmd dpu scale-test [W H S]  one-shot, on its own thread: a W x H (default 1125x2436) test card at splash carve-out + 16 MiB, VIG0
//                                pointed at it with QSEED3 bilinear scaling to the panel, held S seconds (default 15), then everything
//                                restored. `kcmd dpu` afterwards shows the result line (frame counts across the test, restore check).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/libkern.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <stdarg.h>

extern "C" {
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router)
    __attribute__((weak));
// The DPU interrupt (fajita_dpu_vsync_attach): [0] interrupts, [1] PP0 rd_ptr, [2] PP0 done, [3] every status bit seen (OR), [4] the last
// status, [5] the attach result, [6] status-0 interrupts, [7] GIC readback.
volatile uint32_t fajita_dpu_irq[8];
volatile uint64_t fajita_dpu_irq_stamp;   // mach time of the last PP0 rd_ptr
// Flips: [0] requested, [1] latched, [2] refused (one pending, no interrupt, or an address outside the buffers), [3] writes the GPU was
// told to make into the buffer on screen (the tear counter: 0 with flips; see fajita_dpu_scanned).
volatile uint32_t fajita_dpu_flips[4];
extern volatile uint32_t native_legacy_hw_vsync[8] __attribute__((weak));   // NativeLegacyDisplay's hardware vsync counters
}

namespace {
constexpr uint64_t kMdss = 0x0ae00000, kMdssSize = 0x90000;
constexpr uint64_t kCarve = 0x9d400000, kCarveSize = 0x2400000, kTestOffset = 0x1000000;
constexpr uint32_t kVig = 0x5000, kQ = kVig + 0xa00, kCtl = 0x2000, kPp = 0x71000, kIntf = 0x6b800;
OSSharedPtr<IOMemoryMap> gMap;
char gResult[220] = "scale-test: not run";
volatile bool gBusy;

volatile uint32_t *regs() {
    if (!gMap) {
        IODeviceMemory *m = IODeviceMemory::withRange(kMdss, kMdssSize);
        if (!m) return nullptr;
        gMap = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        m->release();
        if (!gMap) return nullptr;
    }
    return reinterpret_cast<volatile uint32_t *>(gMap->getVirtualAddress());
}
uint32_t rd(volatile uint32_t *r, uint32_t off) {return off < kMdssSize ? r[off / 4] : 0xdeadbeef;}
void wr(volatile uint32_t *r, uint32_t off, uint32_t v) {if (off < kMdssSize) r[off / 4] = v; __asm__ volatile("dsb sy" ::: "memory");}

struct Out {
    char *p; size_t cap, n = 0;
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (n + 1 >= cap) return;
        va_list ap; va_start(ap, fmt); int w = vsnprintf(p + n, cap - n, fmt, ap); va_end(ap);
        if (w > 0) n = n + (size_t)w < cap ? n + (size_t)w : cap - 1;
    }
};

uint32_t parse(const char *&s, const char *end) {
    while (s < end && *s == ' ') ++s;
    uint32_t v = 0; int base = 10;
    if (s + 1 < end && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {base = 16; s += 2;}
    for (; s < end; ++s) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : base == 16 && *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : base == 16 && *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * base + (uint32_t)d;
    }
    return v;
}

// The registers the test changes, saved and restored as they were. kQOpIndex: the QSEED3 op mode, restored last before the flush.
const uint32_t kSaved[] = {kVig + 0x00, kVig + 0x08, kVig + 0x0c, kVig + 0x10, kVig + 0x14, kVig + 0x24,
                           kVig + 0x100, kVig + 0x104, kVig + 0x108, kVig + 0x110, kVig + 0x114, kVig + 0x118, kVig + 0x120, kVig + 0x124, kVig + 0x128,
                           kQ + 0x04, kQ + 0x0c, kQ + 0x10, kQ + 0x14, kQ + 0x18, kQ + 0x1c, kQ + 0x20, kQ + 0x40, kQ + 0x44, kQ + 0x48,
                           kQ + 0x90, kQ + 0x94, kQ + 0x98, kQ + 0x9c};
constexpr unsigned kSavedCount = sizeof kSaved / sizeof kSaved[0], kQOpIndex = 15;

// ---- hardware vsync: the MDSS interrupt (sdm845.dtsi mdss: GIC SPI 83, level high = INTID 115). The MDP top's INTR_EN/STATUS/CLEAR
// (MDP + 0x010/0x014/0x018, dpu_hwio.h) fan it out; PINGPONG_0's rd_ptr is bit 12 and its done bit 8 (catalog/dpu_4_0_sdm845.h). Under
// command mode with autorefresh the rd_ptr interrupt fires once per frame the DPU sends to the panel, at PP_RD_PTR_IRQ's line: the
// display's frame clock, in hardware. The handler clears what it read and hands each rd_ptr to the display (an interrupt event source).
constexpr uint32_t kIntrEn = 0x1010, kIntrStatus = 0x1014, kIntrClear = 0x1018, kPpRdPtr = 1u << 12, kPpDone = 1u << 8;
// INTR2 (MDP + 0x008/0x00c/0x02c): CTL0's start (bit 9), the frame start where flushed registers latch: a pending flip retires there,
// a frame earlier than at rd_ptr (the end of the frame's read).
constexpr uint32_t kIntr2En = 0x1008, kIntr2Status = 0x100c, kIntr2Clear = 0x102c, kCtl0Start = 1u << 9;
volatile uint32_t gCtlStarts;
volatile uint32_t *gIrqRegs;
void (*gVsync)(void *, uint64_t);
void *gVsyncContext;
// A flip (fajita_dpu_flip): VIG0's source moved to `phys` and CTL0 flushed (VIG0 | CTL). The DPU takes flushed registers at its next frame
// start; at each rd_ptr (the end of a frame's read) a pending flip whose VIG0 flush bit has cleared has latched: that frame was read from
// the new buffer, so the old one is free, and `latched` is called (interrupt context).
struct Flip {uint64_t phys; void (*latched)(void *); void *context; volatile bool pending;};
Flip gFlip;
volatile bool gTearTest;   // kcmd dpu tear-test owns the scan-out: other flips are refused
uint64_t gScanned = 0;   // the buffer VIG0 reads (0 until the first flip: the firmware's)
void dpuIrq(void *) {
    volatile uint32_t *r = gIrqRegs;
    if (!r) return;
    const uint32_t status = rd(r, kIntrStatus), status2 = rd(r, kIntr2Status);
    wr(r, kIntrClear, status);
    if (status2) wr(r, kIntr2Clear, status2);
    if (status2 & kCtl0Start) {
        gCtlStarts = gCtlStarts + 1;
        if (gFlip.pending && !(rd(r, kCtl + 0x18) & 1u)) {
            gFlip.pending = false;
            gScanned = gFlip.phys;
            fajita_dpu_flips[1] = fajita_dpu_flips[1] + 1;
            if (gFlip.latched) gFlip.latched(gFlip.context);
        }
    }
    fajita_dpu_irq[0] = fajita_dpu_irq[0] + 1;
    fajita_dpu_irq[3] = fajita_dpu_irq[3] | status;
    fajita_dpu_irq[4] = status;
    if (!status && !status2) fajita_dpu_irq[6] = fajita_dpu_irq[6] + 1;
    if (status & kPpDone) fajita_dpu_irq[2] = fajita_dpu_irq[2] + 1;
    if (status & kPpRdPtr) {
        const uint64_t now = mach_absolute_time();
        fajita_dpu_irq_stamp = now;
        fajita_dpu_irq[1] = fajita_dpu_irq[1] + 1;
        if (gFlip.pending && !(rd(r, kCtl + 0x18) & 1u)) {
            gFlip.pending = false;
            gScanned = gFlip.phys;
            fajita_dpu_flips[1] = fajita_dpu_flips[1] + 1;
            if (gFlip.latched) gFlip.latched(gFlip.context);
        }
        if (gVsync) gVsync(gVsyncContext, now);
    }
}

// kcmd dpu te on|off: PINGPONG_0 paced by the panel's TE (gpio10, mdp_vsync; MDP_VSYNC_SEL 0 = GPIO 0) instead of its own counter, with
// Linux's command-mode tear check (dpu_encoder_phys_cmd_tearcheck_config, dpu_hw_pp_enable_te): hw_vsync_mode, sync height twice vtotal,
// init value and start position at vdisplay, rd_ptr at vdisplay + 1, thresholds 4/4, write count start + 5, then TEAR_CHECK_EN. ABL leaves
// the tear check off and the pingpong on its own counter. Self-reverting in the kernel: if no rd_ptr interrupt comes within 200 ms of the
// switch, every saved register is restored (a panel without TE would otherwise stop getting frames). On its own thread; the rd_ptr rate
// over 2 s before and after is the result.
char gTeResult[240] = "te: not run";
constexpr uint32_t kTeRegs[] = {0x04, 0x08, 0x0c, 0x10, 0x18, 0x1c, 0x20, 0x00};   // TEAR_CHECK_EN last
uint32_t gTeSaved[8]; bool gTeHave, gTeOn;
void teRestore(volatile uint32_t *r) {for (unsigned i = 0; i < 8; ++i) wr(r, kPp + kTeRegs[i], gTeSaved[i]);}
void teRun(void *arg, wait_result_t) {
    volatile uint32_t *r = regs();
    const bool on = arg != nullptr;
    const uint32_t a = fajita_dpu_irq[1]; IOSleep(2000); const uint32_t b = fajita_dpu_irq[1];
    bool reverted = false;
    if (on) {
        if (!gTeHave) {for (unsigned i = 0; i < 8; ++i) gTeSaved[i] = rd(r, kPp + kTeRegs[i]); gTeHave = true;}
        const uint32_t vtotal = gTeSaved[1] & 0xffff, vdisplay = rd(r, kVig + 0x0c) >> 16;
        const uint32_t start = vdisplay, threshStart = 4, threshContinue = 4;
        wr(r, kPp + 0x00, 0);
        wr(r, kPp + 0x04, (gTeSaved[0] & 0x7ffff) | (1u << 19) | (1u << 20));
        wr(r, kPp + 0x08, vtotal * 2);
        wr(r, kPp + 0x10, vdisplay);
        wr(r, kPp + 0x20, vdisplay + 1);
        wr(r, kPp + 0x1c, start);
        wr(r, kPp + 0x18, (threshContinue << 16) | threshStart);
        wr(r, kPp + 0x0c, start + threshStart + 1);
        const uint32_t before = fajita_dpu_irq[1];
        wr(r, kPp + 0x00, 1);
        IOSleep(200);
        if (fajita_dpu_irq[1] == before) {teRestore(r); reverted = true;}
    } else if (gTeHave) {
        teRestore(r);
    }
    const uint32_t c = fajita_dpu_irq[1]; IOSleep(2000); const uint32_t d = fajita_dpu_irq[1];
    gTeOn = on && !reverted;
    snprintf(gTeResult, sizeof gTeResult, "te %s: rd_ptr %u/2s before, %u/2s after%s | vsync %08x height %u init %u rd %u start %u en %u",
             on ? "on" : "off", b - a, d - c, reverted ? ", no rd_ptr within 200 ms: restored" : "", rd(r, kPp + 0x04), rd(r, kPp + 0x08),
             rd(r, kPp + 0x10), rd(r, kPp + 0x20), rd(r, kPp + 0x1c), rd(r, kPp + 0x00));
    gBusy = false;
    thread_terminate(current_thread());
}

// kcmd dpu tear-test [SECONDS] [single|flip]: a white bar 64 px wide moving 24 px a frame across black, the whole frame redrawn each time.
// single: drawn straight into the buffer VIG0 reads (as the presenter did before flips: the bar shears where the drawing passes the scan);
// flip: drawn into the other buffer of the carve-out, flipped to at the next frame boundary, the next frame drawn once that latched. For the
// eye (or a camera): our screenshots read memory, not the panel. The presenter's flips are refused meanwhile.
void flush(volatile uint32_t *r);
char gTearResult[160] = "tear-test: not run";
struct TearArgs {uint32_t seconds; bool flip;};
TearArgs gTear;
void tearRun(void *, wait_result_t) {
    volatile uint32_t *r = regs();
    IODeviceMemory *m = IODeviceMemory::withRange(kCarve, kCarveSize);
    OSSharedPtr<IOMemoryMap> carve(m ? m->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr, OSNoRetain);
    if (m) m->release();
    const uint32_t size = r ? rd(r, kVig + 0x00) : 0, W = size & 0xffff, H = size >> 16, stride = r ? rd(r, kVig + 0x24) & 0xffff : 0;
    const uint64_t original = r ? rd(r, kVig + 0x14) : 0;
    if (!carve || !W || !H || stride < W * 4 || (uint64_t)stride * H > kTestOffset || !gIrqRegs) {
        snprintf(gTearResult, sizeof gTearResult, "tear-test: setup failed (map %d %ux%u stride %u irq %d)", carve != nullptr, W, H, stride, gIrqRegs != nullptr);
        gBusy = false; thread_terminate(current_thread()); return;
    }
    gTearTest = true;
    while (gFlip.pending) IOSleep(1);
    const uint64_t end = mach_absolute_time() + (uint64_t)gTear.seconds * 19200000ull;
    uint32_t frames = 0, waits = 0, x = 0;
    while (mach_absolute_time() < end) {
        const uint64_t scanned = rd(r, kVig + 0x14);
        const uint64_t target = !gTear.flip ? scanned : scanned == kCarve ? kCarve + kTestOffset : kCarve;
        volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(carve->getVirtualAddress() + (target - kCarve));
        for (uint32_t y = 0; y < H; ++y) {
            volatile uint32_t *row = reinterpret_cast<volatile uint32_t *>(base + (size_t)y * stride);
            for (uint32_t i = 0; i < W; ++i) row[i] = (i >= x && i < x + 64) ? 0xffffffffu : 0xff000000u;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        if (gTear.flip) {
            gFlip.phys = target; gFlip.latched = nullptr; gFlip.context = nullptr;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            gFlip.pending = true;
            wr(r, kVig + 0x14, (uint32_t)target); flush(r);
            for (unsigned k = 0; gFlip.pending && k < 100; ++k) {IOSleep(1); ++waits;}
        }
        x = (x + 24) % (W > 64 ? W - 64 : 1);
        ++frames;
    }
    // Back to the buffer the presenter left on screen.
    if (gTear.flip && rd(r, kVig + 0x14) != original) {
        gFlip.phys = original; gFlip.latched = nullptr; gFlip.pending = true;
        wr(r, kVig + 0x14, (uint32_t)original); flush(r);
        for (unsigned k = 0; gFlip.pending && k < 100; ++k) IOSleep(1);
    }
    gTearTest = false;
    snprintf(gTearResult, sizeof gTearResult, "tear-test %s: %u frames in %u s, %u ms waiting for latches", gTear.flip ? "flip" : "single", frames,
             gTear.seconds, waits);
    gBusy = false;
    thread_terminate(current_thread());
}

struct TestArgs {uint32_t w, h, seconds;};
TestArgs gArgs;

// INTF1 frame counter (INTF_FRAME_COUNT 0xac): it advances with every frame the interface sends, so it shows the panel kept refreshing.
uint32_t frames(volatile uint32_t *r) {return rd(r, kIntf + 0xac);}
void flush(volatile uint32_t *r) {wr(r, kCtl + 0x18, (1u << 0) | (1u << 17));}   // VIG0 + CTL

void scaleTest(void *, wait_result_t)
{
    volatile uint32_t *r = regs();
    const uint32_t W = gArgs.w, H = gArgs.h, stride = W * 4;
    const uint32_t panel = r ? rd(r, kVig + 0x0c) : 0, pw = panel & 0xffff, ph = panel >> 16;
    IODeviceMemory *m = IODeviceMemory::withRange(kCarve, kCarveSize);
    OSSharedPtr<IOMemoryMap> carve(m ? m->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr, OSNoRetain);
    if (m) m->release();
    if (!r || !carve || !pw || !ph || (uint64_t)stride * H > kCarveSize - kTestOffset) {
        snprintf(gResult, sizeof gResult, "scale-test: setup failed (map %d carve %d panel %ux%u)", r != nullptr, carve != nullptr, pw, ph);
        gBusy = false; thread_terminate(current_thread()); return;
    }
    // The test card, in the scan-out's byte order (B, G, R, X): a 16-pixel red frame on the source's very edges, four colour bars (red,
    // green, blue, white, top to bottom) in the middle half, a white diagonal from the top-left to the bottom-right corner, dark grey elsewhere.
    volatile uint32_t *px = reinterpret_cast<volatile uint32_t *>(carve->getVirtualAddress() + kTestOffset);
    for (uint32_t y = 0; y < H; ++y) {
        volatile uint32_t *row = px + (size_t)y * W;
        const uint32_t dx = (uint32_t)((uint64_t)y * W / H);
        const uint32_t bar = y >= H / 4 && y < H * 3 / 4 ? (y - H / 4) * 4 / (H / 2) : 9;
        for (uint32_t x = 0; x < W; ++x) {
            uint32_t c = 0xff303030u;
            if (bar < 4 && x >= W / 4 && x < W * 3 / 4) c = bar == 0 ? 0xffff0000u : bar == 1 ? 0xff00ff00u : bar == 2 ? 0xff0000ffu : 0xffffffffu;
            if (x + 3 >= dx && x <= dx + 3) c = 0xffffffffu;
            if (x < 16 || y < 16 || x + 16 >= W || y + 16 >= H) c = 0xffff0000u;
            row[x] = c;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t saved[kSavedCount];
    for (unsigned i = 0; i < kSavedCount; ++i) saved[i] = rd(r, kSaved[i]);
    const uint32_t version = rd(r, kQ + 0x00), f0 = frames(r);
    // Source rectangle and fetch (dpu_hw_sspp_setup_rects / _setup_pe_config: the requested pixels are the source size, no extension).
    wr(r, kVig + 0x00, (H << 16) | W);
    wr(r, kVig + 0x08, 0);
    wr(r, kVig + 0x24, stride);
    for (uint32_t c = 0; c < 3; ++c) {wr(r, kVig + 0x100 + 0x10 * c, 0); wr(r, kVig + 0x104 + 0x10 * c, 0); wr(r, kVig + 0x108 + 0x10 * c, (H << 16) | W);}
    // QSEED3 as dpu_plane.c sets it for RGB: bilinear, preload 4/3, phase step (1 << 21) * src / dst, initial phase 0, no LUT, no detail
    // enhancer (dpu_hw_setup_scaler3; op mode: enable, BIL filter for Y/RGB, not DX, blend).
    const uint32_t stepX = (uint32_t)(((uint64_t)W << 21) / pw), stepY = (uint32_t)(((uint64_t)H << 21) / ph);
    if (version == 0x1002) wr(r, kQ + 0x0c, 0);
    else for (uint32_t o = 0x90; o <= 0x9c; o += 4) wr(r, kQ + o, 0);
    wr(r, kQ + 0x10, stepX); wr(r, kQ + 0x14, stepY); wr(r, kQ + 0x18, stepX); wr(r, kQ + 0x1c, stepY);
    wr(r, kQ + 0x20, 4u | (3u << 8) | (4u << 16) | (3u << 24));
    wr(r, kQ + 0x40, (H << 16) | W); wr(r, kQ + 0x44, (H << 16) | W); wr(r, kQ + 0x48, (ph << 16) | pw);
    wr(r, kQ + 0x04, (1u << 0) | (1u << 16) | (1u << 14) | (1u << 31));
    wr(r, kVig + 0x14, (uint32_t)(kCarve + kTestOffset));
    flush(r);
    IOSleep(1000);
    const uint32_t f1 = frames(r), pending = rd(r, kCtl + 0x18), qop = rd(r, kQ + 0x04);
    IOSleep(gArgs.seconds > 1 ? (gArgs.seconds - 1) * 1000 : 0);
    const uint32_t f2 = frames(r);
    for (unsigned i = 0; i < kSavedCount; ++i) if (i != kQOpIndex) wr(r, kSaved[i], saved[i]);
    wr(r, kQ + 0x04, saved[kQOpIndex]);
    flush(r);
    IOSleep(200);
    unsigned restored = 0;
    for (unsigned i = 0; i < kSavedCount; ++i) restored += rd(r, kSaved[i]) == saved[i];
    snprintf(gResult, sizeof gResult, "scale-test %ux%u -> %ux%u qver %x op %x steps %x/%x: frames %u -> %u (+1 s) -> %u (end), flush pending %x, restored %u/%u",
             W, H, pw, ph, version, qop, stepX, stepY, f0, f1, f2, pending, restored, kSavedCount);
    IOLog("fajita-dpu: %s\n", gResult);
    gBusy = false;
    thread_terminate(current_thread());
}

size_t kcmdDpu(const char *args, size_t len, char *out, size_t cap) {
    Out o{out, cap};
    volatile uint32_t *r = regs();
    if (!r) {o.add("dpu: MDSS map failed\n"); return o.n;}
    const char *s = args, *end = args + len;
    while (s < end && *s == ' ') ++s;
    if (end - s >= 10 && !strncmp(s, "scale-test", 10)) {
        s += 10;
        if (gBusy) {o.add("dpu: a scale test is running\n"); return o.n;}
        uint32_t w = parse(s, end), h = parse(s, end), secs = parse(s, end);
        gArgs = {w ? w : 1125, h ? h : 2436, secs ? (secs > 60 ? 60 : secs) : 15};
        if (gArgs.w > 2048 || gArgs.h > 4096 || gArgs.w < 64 || gArgs.h < 64) {o.add("dpu: bad size\n"); return o.n;}
        gBusy = true;
        thread_t t = nullptr;
        if (kernel_thread_start(scaleTest, nullptr, &t) != KERN_SUCCESS) {gBusy = false; o.add("dpu: thread failed\n"); return o.n;}
        thread_deallocate(t);
        o.add("dpu: scale test %ux%u for %u s started\n", gArgs.w, gArgs.h, gArgs.seconds);
        return o.n;
    }
    if (end - s >= 9 && !strncmp(s, "tear-test", 9)) {
        s += 9;
        if (gBusy) {o.add("dpu: busy\n"); return o.n;}
        const uint32_t secs = parse(s, end);
        while (s < end && *s == ' ') ++s;
        gTear = {secs ? (secs > 60 ? 60 : secs) : 10, end - s >= 4 && !strncmp(s, "flip", 4)};
        gBusy = true;
        thread_t t = nullptr;
        if (kernel_thread_start(tearRun, nullptr, &t) != KERN_SUCCESS) {gBusy = false; o.add("dpu: thread failed\n"); return o.n;}
        thread_deallocate(t);
        o.add("dpu: tear-test %s for %u s started\n", gTear.flip ? "flip" : "single", gTear.seconds);
        return o.n;
    }
    if (end - s >= 2 && !strncmp(s, "te", 2)) {
        s += 2; while (s < end && *s == ' ') ++s;
        const bool on = end - s >= 2 && !strncmp(s, "on", 2);
        if (gBusy) {o.add("dpu: busy\n"); return o.n;}
        if (!gIrqRegs) {o.add("dpu: te needs the DPU interrupt (fajita-hw-vsync=1)\n"); return o.n;}
        gBusy = true;
        thread_t t = nullptr;
        if (kernel_thread_start(teRun, on ? (void *)1 : nullptr, &t) != KERN_SUCCESS) {gBusy = false; o.add("dpu: thread failed\n"); return o.n;}
        thread_deallocate(t);
        o.add("dpu: te %s started (result in kcmd dpu after 4 s)\n", on ? "on" : "off");
        return o.n;
    }
    if (s < end && *s == 'r') {
        ++s;
        uint32_t off = parse(s, end) & ~3u, n = parse(s, end);
        if (!n) n = 8;
        if (n > 64) n = 64;
        for (uint32_t i = 0; i < n; ++i) {
            if (!(i % 4)) o.add("%s%05x:", i ? "\n" : "", off + 4 * i);
            o.add(" %08x", rd(r, off + 4 * i));
        }
        o.add("\n");
        return o.n;
    }
    if (s < end && *s == 'q') {
        const uint32_t ssz = rd(r, kQ + 0x40), dsz = rd(r, kQ + 0x48);
        o.add("qseed3 ver %08x op %08x phase %08x step %08x/%08x preload %08x src %ux%u dst %ux%u\n", rd(r, kQ), rd(r, kQ + 0x04),
              rd(r, kQ + 0x0c), rd(r, kQ + 0x10), rd(r, kQ + 0x14), rd(r, kQ + 0x20), ssz & 0xffff, ssz >> 16, dsz & 0xffff, dsz >> 16);
        o.add("pe lr %08x tb %08x req %08x\n", rd(r, kVig + 0x100), rd(r, kVig + 0x104), rd(r, kVig + 0x108));
        return o.n;
    }
    const uint32_t size = rd(r, kVig + 0x00), outsz = rd(r, kVig + 0x0c);
    o.add("VIG0 src %08x %ux%u ystride %u out %ux%u fmt %08x op %08x\n", rd(r, kVig + 0x14), size & 0xffff, size >> 16,
          rd(r, kVig + 0x24) & 0xffff, outsz & 0xffff, outsz >> 16, rd(r, kVig + 0x30), rd(r, kVig + 0x38));
    o.add("CTL0 flush %08x | PP0 autorefresh %08x | INTF1 frames %u\n", rd(r, kCtl + 0x18), rd(r, kPp + 0x30), frames(r));
    o.add("irq: n %u rd_ptr %u done %u seen %08x last %08x zero %u attach %x gic %x | INTR_EN %08x | PP0 rd_ptr frames %u line %u\n",
          fajita_dpu_irq[0], fajita_dpu_irq[1], fajita_dpu_irq[2], fajita_dpu_irq[3], fajita_dpu_irq[4], fajita_dpu_irq[6], fajita_dpu_irq[5],
          fajita_dpu_irq[7], rd(r, kIntrEn), rd(r, kPp + 0x14) >> 16, rd(r, kPp + 0x14) & 0xffff);
    o.add("flips: requested %u latched %u refused %u scanned-buffer writes %u | VIG0 %08x pending %d | ctl0 starts %u INTR2_EN %08x\n",
          fajita_dpu_flips[0], fajita_dpu_flips[1], fajita_dpu_flips[2], fajita_dpu_flips[3], rd(r, kVig + 0x14), (int)gFlip.pending, gCtlStarts,
          rd(r, kIntr2En));
    if (&native_legacy_hw_vsync[0])
        o.add("display vsync: delivered %u sent %u no-client %u coalesced %u attach %d interval %u us\n", native_legacy_hw_vsync[0],
              native_legacy_hw_vsync[1], native_legacy_hw_vsync[2], native_legacy_hw_vsync[3], (int)native_legacy_hw_vsync[4] - 100,
              native_legacy_hw_vsync[5]);
    o.add("%s\n%s\n%s\n", gResult, gTeResult, gTearResult);
    return o.n;
}
}  // namespace

// The display's hardware vsync (NativeLegacyDisplay, fajita-hw-vsync=1): `vsync` is called from the interrupt handler, in interrupt
// context, with the mach time of each PP0 rd_ptr. 0 on success.
extern "C" int fajita_dpu_vsync_attach(void (*vsync)(void *, uint64_t), void *context) {
    volatile uint32_t *r = regs();
    if (!r || !vsync || !armcpu_spi_attach) return -1;
    if (gIrqRegs) return -2;
    gVsyncContext = context; gVsync = vsync; gIrqRegs = r;
    wr(r, kIntrEn, rd(r, kIntrEn) & ~kPpRdPtr);
    wr(r, kIntrClear, 0xffffffffu);
    uint32_t readback = 0; uint64_t router = 0;
    const IOReturn a = armcpu_spi_attach(115, false, dpuIrq, nullptr, &readback, &router);
    fajita_dpu_irq[5] = (uint32_t)a; fajita_dpu_irq[7] = readback;
    if (a != kIOReturnSuccess) {gIrqRegs = nullptr; gVsync = nullptr; return -3;}
    wr(r, kIntrEn, rd(r, kIntrEn) | kPpRdPtr);
    wr(r, kIntr2Clear, kCtl0Start);
    wr(r, kIntr2En, rd(r, kIntr2En) | kCtl0Start);
    // fajita-hw-vsync=2: the panel's TE paces the pingpong from boot (kcmd dpu te on, with its 200 ms self-revert).
    unsigned mode = 0; PE_parse_boot_argn("fajita-hw-vsync", &mode, sizeof mode);
    if (mode >= 2 && !gBusy) {
        gBusy = true;
        thread_t t = nullptr;
        if (kernel_thread_start(teRun, (void *)1, &t) == KERN_SUCCESS) thread_deallocate(t); else gBusy = false;
    }
    return 0;
}

// Double-buffered scan-out (NativeGOPPresenter, fajita-fb-flip=1): the back buffer for a front buffer at `front` of `bytes`, in the
// splash carve-out (at +16 MiB, where the scale test showed VIG0 scans), or 0 when it does not fit or the interrupt is not attached.
extern "C" uint64_t fajita_dpu_flip_back(uint64_t front, uint64_t bytes) {
    if (!gIrqRegs || front != kCarve || kTestOffset + bytes > kCarveSize || bytes > kTestOffset) return 0;
    return kCarve + kTestOffset;
}
static uint64_t gFlipBytes;   // the buffers' size (fajita_dpu_flip_third), so the third buffer's address can be checked
// The third buffer (triple buffering): after the back buffer, MiB-aligned, when it fits in the carve-out; 0 otherwise.
extern "C" uint64_t fajita_dpu_flip_third(uint64_t front, uint64_t bytes) {
    const uint64_t at = (kTestOffset + bytes + 0xfffff) & ~0xfffffull;
    if (!gIrqRegs || front != kCarve || bytes > kTestOffset || at + bytes > kCarveSize) return 0;
    gFlipBytes = bytes;
    return kCarve + at;
}
static bool flipTarget(uint64_t phys, uint64_t bytes) {
    return phys == kCarve || phys == kCarve + kTestOffset || (bytes && phys == kCarve + ((kTestOffset + bytes + 0xfffff) & ~0xfffffull));
}

// The buffer VIG0 is reading now (its source address), for the tear counter.
extern "C" uint64_t fajita_dpu_scanned(void) {
    volatile uint32_t *r = gIrqRegs;
    return r ? rd(r, kVig + 0x14) : 0;
}
extern "C" void fajita_dpu_count_scanned_write(void) {fajita_dpu_flips[3] = fajita_dpu_flips[3] + 1;}
// Show `phys` from the next frame on; `latched` (interrupt context) once the old buffer is no longer read. 0 on success.
extern "C" int fajita_dpu_flip(uint64_t phys, void (*latched)(void *), void *context) {
    volatile uint32_t *r = gIrqRegs;
    if (!r || gFlip.pending || gTearTest || !flipTarget(phys, gFlipBytes)) {fajita_dpu_flips[2] = fajita_dpu_flips[2] + 1; return -1;}
    gFlip.phys = phys; gFlip.latched = latched; gFlip.context = context;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    gFlip.pending = true;
    wr(r, kVig + 0x14, (uint32_t)phys);
    flush(r);
    fajita_dpu_flips[0] = fajita_dpu_flips[0] + 1;
    return 0;
}

// Called by FajitaCpuFreq.cpp's sheng_cpufreq_poll once IOKit is up (same hook as fajita_bus_poll).
extern "C" void fajita_dpu_poll(void) {
    static bool done;
    if (done || !IOService::getPlatform()) return;
    done = true;
    if (fajita_usbdbg_register_kcmd)
        fajita_usbdbg_register_kcmd("dpu", kcmdDpu, "DPU scan-out path: dpu | dpu q | dpu r OFF [N] | dpu scale-test [W H SECONDS] | dpu te on|off | dpu tear-test [S] [single|flip]");
}
