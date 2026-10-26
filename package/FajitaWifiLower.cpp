// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita Wi-Fi: the read-only survey of the layers under the modem link (NOTES-wifi.md "Lower layers, read-only"), run by
// the fajita-wifi worker when `fajita-wifi-ro=1` is given with `fajita-wifi=1`, and on demand by `kcmd wifi`. Default off.
// It reads, and never writes:
//   - SMEM at 0x86000000 (uncached): WifiSmem's parser on the live region (header, version, ptable, the APPS partitions
//     and the global one, their items), and the SMP2P items of every APPS pair (out and in) decoded by wsmp2p_peek.
//     Only the header, the ptable and the partitions whose hosts include APPS or GLOBAL are touched (the others may be
//     XPU-protected from the AP).
//   - cmd-db (fajita_rpmh_lookup): the addresses and ARC level tables of cx.lvl, mx.lvl, mss.lvl, and their current
//     values through an RPMh READ request on an idle apps TCS (fajita_rpmh_send with msgid read: no vote is sent).
//   - AOSS QMP message RAM at 0x0c300000: magic, version, link/channel states of both ends (no doorbell, no message).
// Rows "FWR ..." go to IOLog (and BootLog) and to a text buffer kcmd wifi prints. `kcmd wifi snap` copies those readable
// SMEM ranges into a RAM buffer for scripts/fajita/test/smem-snap.py, so the host tests can run the parser on real data.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include <string.h>

#include "FajitaWifiBoard.h"
extern "C" {
#include "WifiSmem.h"
int fajita_rpmh_lookup(const char *name, uint32_t *addr, uint8_t *aux, unsigned *auxLen) __attribute__((weak));
int fajita_rpmh_send(unsigned n, const uint32_t (*cmd)[3], uint32_t *resp) __attribute__((weak));
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
void fajita_wifi_lower(void);
void fajita_wifi_lower_kcmd(void);
}

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
bool mapRegion(uint64_t phys, uint64_t length, Region &region)
{
    if (region.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
inline uint32_t rd32(uintptr_t a) {return *reinterpret_cast<volatile uint32_t *>(a);}

Region gSmem, gAoss;
IOLock *gLock;
char gText[6144];   // every FWR row of the last survey, '\n'-separated
size_t gTextLen;
uint8_t *gSnap;     // kcmd wifi snap
uint32_t gSnapCrc;

// One row: appended to gText and logged.
char gRow[200];
size_t gRowLen;
void rowStart() {gRowLen = 0; gRow[0] = 0;}
void rowAdd(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rowAdd(const char *fmt, ...)
{
    if (gRowLen + 1 >= sizeof gRow) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(gRow + gRowLen, sizeof gRow - gRowLen, fmt, ap);
    va_end(ap);
    if (n > 0) gRowLen = gRowLen + size_t(n) >= sizeof gRow ? sizeof gRow - 1 : gRowLen + size_t(n);
}
void rowEnd()
{
    IOLog("fajita-wifi: %s\n", gRow);
    if (gTextLen + gRowLen + 2 < sizeof gText) {
        memcpy(gText + gTextLen, gRow, gRowLen);
        gTextLen += gRowLen;
        gText[gTextLen++] = '\n';
        gText[gTextLen] = 0;
    }
    rowStart();
}
// Long lists wrap onto continuation rows: call before adding the next element.
void rowWrap(const char *cont) {if (gRowLen > 140) {rowEnd(); rowAdd("%s", cont);}}

// SMEM host ids (Qualcomm's smem host numbering; smem.c names 0 APPS, 1 MODEM; 0xfffe is the global partition and 0xffff
// marks the ptable's own entries for the header and the table).
const char *hostName(unsigned h)
{
    static const char *const names[] = {"apps", "modem", "adsp", "slpi", "wcnss", "cdsp", "rpm", "tz", "spss", "hyp"};
    static char other[4][8];
    static unsigned next;
    if (h == WSMEM_GLOBAL_HOST) return "global";
    if (h < sizeof names / sizeof names[0]) return names[h];
    char *o = other[next++ & 3];
    snprintf(o, sizeof other[0], "%x", h);
    return o;
}

struct ItemCtx {unsigned n;};
void itemRow(void *ctx, unsigned item, uint32_t off, uint32_t size, int cached)
{
    ItemCtx *c = static_cast<ItemCtx *>(ctx);
    rowWrap("FWR   ...");
    rowAdd(" %u@%x+%x%s", item, off, size, cached ? "c" : "");
    ++c->n;
}

void surveySmem()
{
    rowStart();
    if (!mapRegion(FW_SMEM_PHYS, FW_SMEM_SIZE, gSmem)) {rowAdd("FWR smem map FAILED"); rowEnd(); return;}
    const uintptr_t b = gSmem.va;
    wsmem s;
    const int r = wsmem_init(&s, reinterpret_cast<volatile void *>(b), FW_SMEM_SIZE, WSMEM_HOST_APPS);
    // struct smem_header: version[32] @64, initialized @192, free_offset @196, available @200, reserved @204.
    rowAdd("FWR smem @%llx init %d sbl-ver %08x (v%u) initialized %u heap free %x avail %x items %u parts %u global %x+%x",
           (unsigned long long)FW_SMEM_PHYS, r, rd32(b + 64 + 4 * WSMEM_SBL_VERSION_INDEX), s.version, rd32(b + 192),
           rd32(b + 196), rd32(b + 200), s.item_count, s.nparts, s.global.offset, s.global.size);
    rowEnd();
    const unsigned n = wsmem_ptable_count(&s);
    rowAdd("FWR ptable %u:", n);
    for (unsigned i = 0; i < n; ++i) {
        wsmem_ptentry e;
        if (wsmem_ptable_entry(&s, i, &e)) break;
        rowWrap("FWR   ptable");
        rowAdd(" %s-%s@%x+%x/cl%x/f%x", hostName(e.host0), hostName(e.host1), e.offset, e.size, e.cacheline, e.flags);
    }
    rowEnd();
    if (r) return;
    // The partitions the parser kept (APPS pairs), then the global one.
    for (unsigned h = 0; h <= WSMEM_HOST_COUNT; ++h) {
        const unsigned host = h == WSMEM_HOST_COUNT ? WSMEM_GLOBAL_HOST : h;
        const wsmem_part &p = host == WSMEM_GLOBAL_HOST ? s.global : s.part[h];
        if (!p.size) continue;
        rowAdd("FWR part apps-%s @%x+%x cl%x free-unc %x free-cached %x space %ld items:", hostName(host), p.offset, p.size,
               p.cacheline, rd32(b + p.offset + 12), rd32(b + p.offset + 16), wsmem_free_space(&s, host));
        ItemCtx c = {0};
        const int k = wsmem_items(&s, host, itemRow, &c);
        rowAdd(" = %d", k);
        rowEnd();
    }
    // SMP2P, every APPS pair of sdm845.dtsi (local pid 0): {remote pid, out item, in item}.
    static const struct {const char *name; unsigned pid, out, in;} kPairs[] = {
        {"mpss", 1, 435, 428}, {"lpass", 2, 443, 429}, {"slpi", 3, 481, 430}, {"cdsp", 5, 94, 432}};
    for (const auto &pr : kPairs) {
        for (unsigned dir = 0; dir < 2; ++dir) {
            const unsigned item = dir ? pr.in : pr.out;
            size_t sz = 0;
            int err = 0;
            volatile void *it = wsmem_get(&s, pr.pid, item, &sz, &err);
            rowAdd("FWR smp2p %s %s item %u", pr.name, dir ? "in" : "out", item);
            if (!it) {rowAdd(" absent(%d)", err); rowEnd(); continue;}
            wsmp2p_view v;
            const int pk = wsmp2p_peek(it, sz, &v);
            rowAdd(" @%lx+%lx peek %d magic %08x v%u feat %x pid %u->%u entries %u/%u flags %x", wsmem_offset(&s, it),
                   (unsigned long)sz, pk, v.magic, v.version, v.features, v.local_pid, v.remote_pid, v.valid, v.total, v.flags);
            for (unsigned i = 0; i < v.nentries; ++i) {rowWrap("FWR   smp2p"); rowAdd(" %s=%x", v.entry[i].name, v.entry[i].value);}
            rowEnd();
        }
    }
}

void surveyRpmh()
{
    static const char *const kRes[] = {"cx.lvl", "mx.lvl", "mss.lvl"};
    uint32_t cmd[3][3], resp[3] = {};
    unsigned found = 0;
    rowStart();
    if (!fajita_rpmh_lookup) {rowAdd("FWR rpmh: no fajita_rpmh_lookup (FajitaBusVote.cpp)"); rowEnd(); return;}
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t addr = 0;
        uint8_t aux[32] = {};
        unsigned auxLen = sizeof aux;
        const int r = fajita_rpmh_lookup(kRes[i], &addr, aux, &auxLen);
        rowAdd("FWR rpmh %s lookup %d addr %x aux %u B levels", kRes[i], r, addr, auxLen);
        for (unsigned k = 0; k + 1 < auxLen; k += 2) rowAdd(" %u", unsigned(aux[k]) | unsigned(aux[k + 1]) << 8);
        rowEnd();
        if (!r && addr) {cmd[found][0] = (1u << 8) | 8u; cmd[found][1] = addr; cmd[found][2] = 0; ++found;}   // READ + resp
    }
    if (!found) return;
    // RPMh READ (msgid without the write bit): the TCS read-back FajitaBusVote's `bus read` uses; no vote is sent.
    const int t = fajita_rpmh_send ? fajita_rpmh_send(found, cmd, resp) : -19;
    rowAdd("FWR rpmh read tcs%d", t);
    for (unsigned i = 0; i < found; ++i) rowAdd(" %x=%08x", cmd[i][1], resp[i]);
    rowEnd();
}

void surveyAoss()
{
    rowStart();
    if (!mapRegion(FW_AOSS_QMP_PHYS, FW_AOSS_QMP_SIZE, gAoss)) {rowAdd("FWR aoss map FAILED"); rowEnd(); return;}
    const uintptr_t q = gAoss.va;
    // qcom_aoss.c QMP_DESC_*: magic 0, version 4, features 8; ucore (AOSS) link/ack/ch/ack/mbox size/off at 0xc..0x20,
    // mcore (APSS) at 0x24..0x38. Link and channel are up at 0xffff (QMP_STATE_UP), down at 0xffff0000.
    const uint32_t magic = rd32(q + 0), ver = rd32(q + 4);
    rowAdd("FWR aoss @%llx magic %08x(%s) ver %u feat %x | ucore link %x/%x ch %x/%x mbox %x+%x | mcore link %x/%x ch %x/%x mbox %x+%x",
           (unsigned long long)FW_AOSS_QMP_PHYS, magic, magic == 0x4d41494cu ? "ok" : "BAD", ver, rd32(q + 8), rd32(q + 0xc),
           rd32(q + 0x10), rd32(q + 0x14), rd32(q + 0x18), rd32(q + 0x20), rd32(q + 0x1c), rd32(q + 0x24), rd32(q + 0x28),
           rd32(q + 0x2c), rd32(q + 0x30), rd32(q + 0x38), rd32(q + 0x34));
    rowEnd();
    const uint32_t off = rd32(q + 0x38), size = rd32(q + 0x34);
    if (off && off + 4 <= FW_AOSS_QMP_SIZE && size <= FW_AOSS_QMP_SIZE) {
        rowAdd("FWR aoss mcore mbox[0] len %x (0 = no message pending)", rd32(q + off));
        rowEnd();
    }
}

void survey()
{
    gTextLen = 0;
    gText[0] = 0;
    surveySmem();
    surveyRpmh();
    surveyAoss();
}

uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (unsigned k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

// Copy the AP-readable SMEM ranges into a 2 MiB RAM buffer (the rest stays zero), with 32-bit loads.
size_t snap(char *out, size_t cap)
{
    if (!mapRegion(FW_SMEM_PHYS, FW_SMEM_SIZE, gSmem)) return snprintf(out, cap, "wifi snap: smem map failed\n");
    if (!gSnap) gSnap = static_cast<uint8_t *>(IOMallocData(FW_SMEM_SIZE));
    if (!gSnap) return snprintf(out, cap, "wifi snap: no memory\n");
    memset(gSnap, 0, FW_SMEM_SIZE);
    wsmem s;
    const int r = wsmem_init(&s, reinterpret_cast<volatile void *>(gSmem.va), FW_SMEM_SIZE, WSMEM_HOST_APPS);
    struct {uint32_t off, size;} ranges[WSMEM_HOST_COUNT + 3];
    unsigned nr = 0;
    ranges[nr++] = {0, (WSMEM_HEADER_BYTES + 15u) & ~15u};
    ranges[nr++] = {uint32_t(FW_SMEM_SIZE - WSMEM_PTABLE_BYTES), WSMEM_PTABLE_BYTES};
    if (!r) {
        if (s.global.size) ranges[nr++] = {s.global.offset, s.global.size};
        for (unsigned h = 0; h < WSMEM_HOST_COUNT; ++h) if (s.part[h].size) ranges[nr++] = {s.part[h].offset, s.part[h].size};
    }
    int at = snprintf(out, cap, "ok va=0x%llx bytes=%u init=%d ranges=", (unsigned long long)reinterpret_cast<uintptr_t>(gSnap),
                      unsigned(FW_SMEM_SIZE), r);
    for (unsigned i = 0; i < nr; ++i) {
        const uint32_t o = ranges[i].off & ~3u, e = (ranges[i].off + ranges[i].size + 3) & ~3u;
        if (e > FW_SMEM_SIZE || o >= e) continue;
        for (uint32_t a = o; a < e; a += 4) {const uint32_t w = rd32(gSmem.va + a); memcpy(gSnap + a, &w, 4);}
        if (at > 0 && size_t(at) < cap) at += snprintf(out + at, cap - at, "%s%x+%x", i ? "," : "", o, e - o);
    }
    gSnapCrc = crc32(gSnap, FW_SMEM_SIZE);
    if (at > 0 && size_t(at) < cap) at += snprintf(out + at, cap - at, " crc=%08x\n", gSnapCrc);
    return at > 0 ? (size_t(at) < cap ? size_t(at) : cap) : 0;
}

// Rows of the last survey from line `from`, as many as fit; "more N" when cut.
size_t rows(unsigned from, char *out, size_t cap)
{
    size_t at = 0;
    unsigned line = 0;
    for (size_t i = 0; i < gTextLen;) {
        size_t e = i;
        while (e < gTextLen && gText[e] != '\n') ++e;
        if (line >= from) {
            if (at + (e - i) + 1 + 16 >= cap) {at += snprintf(out + at, cap - at, "more %u\n", line); return at;}
            memcpy(out + at, gText + i, e - i);
            at += e - i;
            out[at++] = '\n';
        }
        ++line;
        i = e + 1;
    }
    if (at < cap) out[at] = 0;
    return at;
}

size_t kcmdWifi(const char *args, size_t len, char *out, size_t cap)
{
    char a[32] = {};
    for (size_t i = 0; i < len && i < sizeof a - 1; ++i) a[i] = args[i];
    unsigned from = 0;
    const char *p = a;
    while (*p && *p != ' ') ++p;
    while (*p == ' ') ++p;
    while (*p >= '0' && *p <= '9') from = from * 10 + unsigned(*p++ - '0');
    if (!gLock) return snprintf(out, cap, "wifi: not ready\n");
    IOLockLock(gLock);
    size_t n;
    if (!strncmp(a, "rows", 4)) n = rows(from, out, cap);
    else if (!strncmp(a, "ro", 2)) {survey(); n = rows(from, out, cap);}
    else if (!strncmp(a, "snap", 4)) n = snap(out, cap);
    else n = snprintf(out, cap, "usage: wifi ro [FROM] (re-run the read-only survey) | rows [FROM] | snap (SMEM copy for readv)\n");
    IOLockUnlock(gLock);
    return n < cap ? n : cap;
}
}  // namespace

// The fajita-wifi worker (FajitaWifiProbe.cpp), fajita-wifi >= 1: register `kcmd wifi`.
extern "C" void fajita_wifi_lower_kcmd(void)
{
    if (!gLock) gLock = IOLockAlloc();
    if (fajita_usbdbg_register_kcmd)
        fajita_usbdbg_register_kcmd("wifi", kcmdWifi, "Wi-Fi lower layers, read-only: wifi ro [FROM] | rows [FROM] | snap");
}

// The fajita-wifi worker, with fajita-wifi-ro=1: one survey at boot.
extern "C" void fajita_wifi_lower(void)
{
    fajita_wifi_lower_kcmd();
    if (!gLock) return;
    IOLockLock(gLock);
    survey();
    IOLockUnlock(gLock);
}
