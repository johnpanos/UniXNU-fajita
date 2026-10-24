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
#include <string.h>
#include <stdarg.h>

extern "C" {
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
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
    o.add("%s\n", gResult);
    return o.n;
}
}  // namespace

// Called by FajitaCpuFreq.cpp's sheng_cpufreq_poll once IOKit is up (same hook as fajita_bus_poll).
extern "C" void fajita_dpu_poll(void) {
    static bool done;
    if (done || !IOService::getPlatform()) return;
    done = true;
    if (fajita_usbdbg_register_kcmd)
        fajita_usbdbg_register_kcmd("dpu", kcmdDpu, "DPU scan-out path: dpu | dpu q | dpu r OFF [N] | dpu scale-test [W H SECONDS]");
}
