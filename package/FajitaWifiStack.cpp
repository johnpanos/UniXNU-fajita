// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita Wi-Fi: everything above a booted modem (NOTES-wifi.md stages 3..), run by the fajita-wifi worker once the modem
// is up (fajita_wifi_stack, called from FajitaWifiProbe.cpp for fajita-wifi >= 3). The protocol code is the freestanding,
// host-tested drivers/FajitaWifi/* (scripts/check-fajita-wifi-host.py); this file only supplies the hardware glue:
//   - SMEM at 0x86000000 (uncached), the TCSR mutex 3 around SMEM allocations, the APCS IPC doorbells
//   - SMP2P with the modem (exported for the modem boot: fajita_wifi_smp2p_bits)
//   - GLINK on SMEM to the modem, QRTR on its IPCRTR channel
//   - stage 4: pd-mapper (.jsn from the modem partition), TFTP (read-only modem partition), rmtfs (read-only EFS)
// Single thread: one poll loop owns every protocol object, so none of them need locks. Rows FW3/FW4 go to
// fajita_wifi_rows[4..5] and IOLog whenever they change.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>

#include "FajitaWifiBoard.h"
extern "C" {
#include "WifiSmem.h"
#include "WifiGlink.h"
#include "WifiRouter.h"
#include "WifiPdmap.h"
#include "WifiTftp.h"
#include "WifiRmtfs.h"
#include "WifiFat.h"
#include "WifiWlfw.h"
extern char fajita_wifi_rows[8][160];
extern volatile uint32_t fajita_wifi_state[8];
int native_ufs_part_find(const char *name, uint8_t *lun, uint64_t *first, uint64_t *blocks, uint64_t *attr) __attribute__((weak));
int native_ufs_part_read(uint8_t lun, uint64_t lba, uint32_t count, void *dst) __attribute__((weak));
// The modem-boot side (memory assignment is theirs): hand the wlfw MSA regions HLOS -> MSS_MSA + WLAN, as
// ath10k_qmi_setup_msa_permissions does. 0 on success. Without it stage 5 stops before msa_ready (FW5 "msa-assign?").
int fajita_wifi_msa_assign(const wlfw_mem_region *regions, unsigned count) __attribute__((weak));
// ld64 does not take an undefined weak reference out of a static archive: this weak default (-78, ENOSYS) stands in
// until the modem-boot side links its strong definition.
__attribute__((weak)) int fajita_wifi_msa_assign(const wlfw_mem_region *, unsigned) {return -78;}
}

// Not in FajitaWifiBoard.h (kept separate from the modem-boot constants):
#define FWS_TCSR_MUTEX_PHYS 0x01f40000ull   // sdm845.dtsi tcsr_mutex: hwlock@1f40000, 0x20000; qcom,tcsr-mutex stride 0x1000
#define FWS_TCSR_MUTEX_STRIDE 0x1000u
#define FWS_SMEM_HWLOCK 3u                  // smem: hwlocks = <&tcsr_mutex 3>
#define FWS_MUTEX_APPS_PROC_ID 1u           // qcom_hwspinlock.c QCOM_MUTEX_APPS_PROC_ID
#define FWS_IPC_GLINK_BIT 12u               // glink-edge mboxes = <&apss_shared 12>
#define FWS_IPC_SMP2P_BIT 14u               // smp2p-mpss mboxes = <&apss_shared 14>

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
bool mapRegion(uint64_t phys, uint64_t length, Region &region)
{
    if (region.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
inline void wr32(uintptr_t a, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(a) = v;}
inline uint32_t rd32(uintptr_t a) {return *reinterpret_cast<volatile uint32_t *>(a);}
uint64_t nowMs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}

Region gSmemMap, gMutexMap, gIpcMap, gRmtfsMap;
wsmem gSmem;
wsmp2p gSmp2p;
int gSmp2pOut = -1;
bool gSmemUp, gSmp2pUp;
IOLock *gInitLock;

// qcom_hwspinlock: write our proc id, read it back.
int hwlock(void *) {
    const uintptr_t r = gMutexMap.va + FWS_SMEM_HWLOCK * FWS_TCSR_MUTEX_STRIDE;
    for (unsigned i = 0; i < 100000; ++i) {
        wr32(r, FWS_MUTEX_APPS_PROC_ID);
        __asm__ volatile("dsb sy" ::: "memory");
        if (rd32(r) == FWS_MUTEX_APPS_PROC_ID) return 0;
        IODelay(1);
    }
    return -1;
}
void hwunlock(void *) {
    __asm__ volatile("dsb sy" ::: "memory");
    wr32(gMutexMap.va + FWS_SMEM_HWLOCK * FWS_TCSR_MUTEX_STRIDE, 0);
}
void ipcKick(uint32_t bit) {
    __asm__ volatile("dsb sy" ::: "memory");
    wr32(gIpcMap.va + FW_APSS_IPC_OFFSET, 1u << bit);
}
void kickGlink(void *) {ipcKick(FWS_IPC_GLINK_BIT);}
void kickSmp2p(void *) {ipcKick(FWS_IPC_SMP2P_BIT);}
void *kalloc(size_t n, void *) {return IOMalloc(n);}
void kfree(void *p, size_t n, void *) {if (p) IOFree(p, n);}

bool smemUp()
{
    if (gSmemUp) return true;
    if (!mapRegion(FW_SMEM_PHYS, FW_SMEM_SIZE, gSmemMap) || !mapRegion(FWS_TCSR_MUTEX_PHYS, 0x20000, gMutexMap)
        || !mapRegion(FW_APSS_SHARED_PHYS, 0x1000, gIpcMap)) return false;
    const int r = wsmem_init(&gSmem, reinterpret_cast<volatile void *>(gSmemMap.va), FW_SMEM_SIZE, WSMEM_HOST_APPS);
    if (r) {IOLog("fajita-wifi: FW3 smem init %d\n", r); return false;}
    gSmem.lock = hwlock;
    gSmem.unlock = hwunlock;
    gSmemUp = true;
    return true;
}
bool smp2pUp()
{
    if (gSmp2pUp) return true;
    if (!smemUp()) return false;
    if (wsmp2p_init(&gSmp2p, &gSmem, 0, 1, FW_SMP2P_MPSS_OUT_ITEM, FW_SMP2P_MPSS_IN_ITEM, kickSmp2p, nullptr)) return false;
    gSmp2pOut = wsmp2p_add_entry(&gSmp2p, "master-kernel");
    gSmp2pUp = gSmp2pOut >= 0;
    return gSmp2pUp;
}

// ---- the modem partition, read-only, through NativeUfs (native-ufs-partread=1) ----
struct UfsPart {char name[16]; uint8_t lun; uint64_t first, blocks; bool ok;};
UfsPart gModemPart, gEfs[8];
uint8_t *gBounce;   // 4 KiB
wfat gFat;
bool gFatUp;

bool partFind(const char *name, UfsPart &p)
{
    if (!native_ufs_part_find) return false;
    uint64_t attr = 0;
    strlcpy(p.name, name, sizeof p.name);
    p.ok = native_ufs_part_find(name, &p.lun, &p.first, &p.blocks, &attr) == 1;
    return p.ok;
}
int fatRead(void *ctx, uint64_t block, uint32_t count, void *dst)
{
    const UfsPart *p = static_cast<UfsPart *>(ctx);
    if (block + count > p->blocks || !native_ufs_part_read) return -1;
    return native_ufs_part_read(p->lun, p->first + block, count, dst) == 1 ? 0 : -1;
}
bool fatUp()
{
    if (gFatUp) return true;
    // the slot: fajita-wifi-slot=a|b, else the modem_a/modem_b GPT active bit (50), else a
    char slot[4] = {0};
    if (!PE_parse_boot_argn("fajita-wifi-slot", slot, sizeof slot)) {
        uint8_t lun; uint64_t f, b, attr = 0;
        slot[0] = 'a';
        if (native_ufs_part_find && native_ufs_part_find("modem_b", &lun, &f, &b, &attr) == 1 && (attr >> 50 & 1)) slot[0] = 'b';
    }
    char name[16];
    snprintf(name, sizeof name, "modem_%c", slot[0] == 'b' ? 'b' : 'a');
    if (!partFind(name, gModemPart)) {IOLog("fajita-wifi: FW4 no %s (native-ufs-partread=1?)\n", name); return false;}
    const int r = wfat_mount(&gFat, fatRead, &gModemPart, gModemPart.blocks);
    if (r) {IOLog("fajita-wifi: FW4 %s FAT mount %d\n", name, r); return false;}
    gFatUp = true;
    return true;
}
int tftpOpen(void *, const char *path, uint64_t *cookie, uint32_t *size)
{
    wfat_file f;
    if (!gFatUp || wfat_open(&gFat, path, &f) || f.dir) return -1;
    *cookie = f.first_cluster | static_cast<uint64_t>(f.size) << 32;
    *size = f.size;
    return 0;
}
long tftpRead(void *, uint64_t cookie, uint32_t off, void *dst, uint32_t len)
{
    wfat_file f = {static_cast<uint32_t>(cookie), static_cast<uint32_t>(cookie >> 32), 0};
    return wfat_read(&gFat, &f, off, dst, len);
}
UfsPart *efsPart(const char *name)
{
    for (UfsPart &p : gEfs) if (p.ok && !strcmp(p.name, name)) return &p;
    for (UfsPart &p : gEfs) if (!p.ok && !p.name[0]) return partFind(name, p) ? &p : nullptr;
    return nullptr;
}
long long efsSize(void *, const char *name)
{
    UfsPart *p = efsPart(name);
    return p ? static_cast<long long>(p->blocks * 4096) : -1;
}
long efsRead(void *, const char *name, uint64_t off, void *dst, uint32_t len)
{
    UfsPart *p = efsPart(name);
    if (!p || !gBounce || !native_ufs_part_read) return -1;
    uint8_t *out = static_cast<uint8_t *>(dst);
    long done = 0;
    while (len) {
        const uint64_t blk = off / 4096;
        if (blk >= p->blocks) break;
        if (native_ufs_part_read(p->lun, p->first + blk, 1, gBounce) != 1) return -1;
        const uint32_t in = static_cast<uint32_t>(off % 4096), n = len < 4096 - in ? len : 4096 - in;
        memcpy(out + done, gBounce + in, n);
        done += n; off += n; len -= n;
    }
    return done;
}

// ---- the link: GLINK IPCRTR <-> QRTR ----
wglink gGlink;
wqrtr gQrtr;
int gIpcrtr = -1;
wpdm gPdm;
wtftp gTftp;
wrmtfs gRmtfs;
uint32_t gPdmPort;
wwlfw gWlfw;
bool gWlfwOn, gEnableSent;
uint8_t *gBoard;
uint32_t gBoardLen;
char gBoardName[24];

int gMsaRc;
int msaAssign(void *, const wlfw_mem_region *regions, unsigned count)
{
    gMsaRc = fajita_wifi_msa_assign(regions, count);
    return gMsaRc;
}
// cnss naming of the modem partition's board files: bdwlan.bin (id 0xff), bdwlan.bNN (< 0xff), bdwlan.NNN.
int boardData(void *, uint32_t board_id, uint32_t, const uint8_t **data, uint32_t *len)
{
    if (board_id == 0xff) snprintf(gBoardName, sizeof gBoardName, "image/bdwlan.bin");
    else if (board_id < 0xff) snprintf(gBoardName, sizeof gBoardName, "image/bdwlan.b%02x", board_id);
    else snprintf(gBoardName, sizeof gBoardName, "image/bdwlan.%03x", board_id & 0xfff);
    wfat_file f;
    if (!gFatUp || wfat_open(&gFat, gBoardName, &f) || !f.size || f.size > 0x10000) return -1;
    if (gBoard && gBoardLen) IOFree(gBoard, gBoardLen);
    gBoard = static_cast<uint8_t *>(IOMalloc(f.size));
    if (!gBoard) return -1;
    gBoardLen = f.size;
    if (wfat_read(&gFat, &f, 0, gBoard, f.size) != static_cast<long>(f.size)) return -1;
    *data = gBoard;
    *len = gBoardLen;
    return 0;
}
struct Seen {uint32_t service, instance, node, port;} gSeen[24];
unsigned gNSeen;

int qrtrXmit(void *, const uint8_t *pkt, size_t len)
{
    if (gIpcrtr < 0 || !wglink_ready(&gGlink, gIpcrtr)) return WQR_ERR_AGAIN;
    return wglink_send(&gGlink, gIpcrtr, pkt, len) == WGLINK_OK ? 0 : WQR_ERR_AGAIN;
}
void ipcrtrRx(wglink *, int, const uint8_t *data, size_t len, void *) {wqrtr_input(&gQrtr, data, len);}
void routerEvent(wqrtr *, int what, uint32_t service, uint32_t instance, uint32_t node, uint32_t port, void *)
{
    if (what == WQR_EV_NEW_SERVER && gNSeen < 24) gSeen[gNSeen++] = {service, instance, node, port};
    if (gWlfwOn && service == WWLFW_SERVICE && instance == WWLFW_INSTANCE) {
        if (what == WQR_EV_NEW_SERVER) {gEnableSent = false; wwlfw_server(&gWlfw, node, port, nowMs());}
        if (what == WQR_EV_DEL_SERVER) wwlfw_server_gone(&gWlfw);
    }
    if (gWlfwOn && what == WQR_EV_BYE) wwlfw_server_gone(&gWlfw);
    if (what == WQR_EV_NEW_SERVER || what == WQR_EV_DEL_SERVER || what == WQR_EV_BYE)
        IOLog("fajita-wifi: FW3 qrtr %s svc 0x%x inst 0x%x node %u port 0x%x\n",
              what == WQR_EV_NEW_SERVER ? "new" : what == WQR_EV_BYE ? "bye" : "del", service, instance, node, port);
    wtftp_event(&gTftp, what, node, port);
    wrmtfs_event(&gRmtfs, what, node);
}
void pdmRx(wqrtr *r, uint32_t port, uint32_t node, uint32_t sport, const uint8_t *b, size_t len, void *)
{
    uint8_t out[1024];
    const long n = wpdm_handle(&gPdm, b, len, out, sizeof out);
    if (n > 0) wqrtr_send(r, port, node, sport, out, static_cast<size_t>(n));
}

bool servicesUp()
{
    gBounce = static_cast<uint8_t *>(IOMallocAligned(4096, 4096));
    const bool fat = fatUp();
    wpdm_init(&gPdm);
    unsigned jsn = 0;
    if (fat) {
        static const char *const files[] = {"image/modemr.jsn", "image/modemuw.jsn"};
        for (const char *f : files) {
            wfat_file wf;
            char buf[2048];
            if (wfat_open(&gFat, f, &wf)) continue;
            const long n = wfat_read(&gFat, &wf, 0, buf, sizeof buf);
            if (n > 0 && wpdm_add_jsn(&gPdm, buf, static_cast<size_t>(n)) > 0) ++jsn;
        }
    }
    if (!jsn) {   // the same table, from Linux's qcom_pd_mapper.c sdm845 entries, if the partition is unreadable
        static const char kRoot[] = "{\"sr_domain\":{\"soc\":\"msm\",\"domain\":\"modem\",\"subdomain\":\"root_pd\",\"qmi_instance_id\":180},"
                                    "\"sr_service\":[{\"provider\":\"tms\",\"service\":\"servreg\"}]}";
        static const char kWlan[] = "{\"sr_domain\":{\"soc\":\"msm\",\"domain\":\"modem\",\"subdomain\":\"wlan_pd\",\"qmi_instance_id\":180},"
                                    "\"sr_service\":[{\"provider\":\"kernel\",\"service\":\"elf_loader\"},{\"provider\":\"tms\",\"service\":\"servreg\"},"
                                    "{\"provider\":\"wlan\",\"service\":\"fw\"}]}";
        wpdm_add_jsn(&gPdm, kRoot, sizeof kRoot - 1);
        wpdm_add_jsn(&gPdm, kWlan, sizeof kWlan - 1);
    }
    gPdmPort = wqrtr_bind(&gQrtr, 0, pdmRx, nullptr);
    wqrtr_publish(&gQrtr, gPdmPort, WPDM_SERVICE, WPDM_VERSION, WPDM_INSTANCE);
    const wtftp_fs fs = {tftpOpen, tftpRead, nullptr};
    wtftp_init(&gTftp, &gQrtr, &fs);
    const bool rm = mapRegion(FW_RMTFS_PHYS, FW_RMTFS_SIZE, gRmtfsMap);
    wrmtfs_init(&gRmtfs, &gQrtr, efsRead, efsSize, nullptr, rm ? reinterpret_cast<volatile uint8_t *>(gRmtfsMap.va) : nullptr,
                FW_RMTFS_PHYS, rm ? static_cast<uint32_t>(FW_RMTFS_SIZE) : 0);
    IOLog("fajita-wifi: FW4 services: pd-mapper %u maps (%s), tftp port 0x%x (%s), rmtfs port 0x%x buf %s\n", gPdm.count,
          jsn ? "modem partition .jsn" : "built-in table", gTftp.port, fat ? gModemPart.name : "NO PARTITION", gRmtfs.port,
          rm ? "mapped" : "UNMAPPED");
    return true;
}

void row(unsigned i, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void row(unsigned i, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (strcmp(buf, fajita_wifi_rows[i])) {
        strlcpy(fajita_wifi_rows[i], buf, sizeof fajita_wifi_rows[i]);
        IOLog("fajita-wifi: %s\n", buf);
    }
}
}  // namespace

// For the modem boot (stage 2): the modem's SMP2P inbound bits (WSMP2P_MSS_READY = bit 2, HANDOVER 3, FATAL 1), 0 once
// present, negative while SMEM/SMP2P are not up or the modem has not allocated its item. Polls; safe to call repeatedly.
extern "C" int fajita_wifi_smp2p_bits(uint32_t *value)
{
    if (!gInitLock) gInitLock = IOLockAlloc();
    IOLockLock(gInitLock);
    int r = -1;
    if (smp2pUp() && wsmp2p_irq(&gSmp2p) == 1) {uint32_t ch; r = wsmp2p_take(&gSmp2p, "slave-kernel", value, &ch) ? -2 : 0;}
    IOLockUnlock(gInitLock);
    return r;
}
// The AP's outbound SMP2P "master-kernel" bits (bit 0 = stop, qcom,smem-states).
extern "C" int fajita_wifi_smp2p_set(uint32_t mask, uint32_t value)
{
    if (!gInitLock) gInitLock = IOLockAlloc();
    IOLockLock(gInitLock);
    const int r = smp2pUp() ? wsmp2p_update(&gSmp2p, gSmp2pOut, mask, value) : -1;
    IOLockUnlock(gInitLock);
    return r;
}

// Stage 3 (SMEM + GLINK + QRTR) and 4 (the AP services); never returns while the modem link is serviced.
extern "C" void fajita_wifi_stack(unsigned stage)
{
    if (!gInitLock) gInitLock = IOLockAlloc();
    IOLockLock(gInitLock);
    const bool smem = smemUp(), smp2p = smem && smp2pUp();
    IOLockUnlock(gInitLock);
    if (!smem) {row(4, "FW3 smem FAILED (map or header)"); return;}
    row(4, "FW3 smem v%u items %u parts %u global %ukB smp2p %s", gSmem.version, gSmem.item_count, gSmem.nparts,
        gSmem.global.size >> 10, smp2p ? "up" : "FAILED");
    if (wglink_smem_init(&gGlink, &gSmem, FW_GLINK_MODEM_PID, kickGlink, nullptr, kalloc, kfree, nullptr)) {
        row(4, "FW3 glink smem items FAILED");
        return;
    }
    wqrtr_init(&gQrtr, qrtrXmit, nullptr, kalloc, kfree, nullptr);
    gQrtr.event = routerEvent;
    gIpcrtr = wglink_open(&gGlink, "IPCRTR", ipcrtrRx, nullptr, 1024, 5);   // qrtr smd.c: the IPCRTR channel
    if (stage >= 4) servicesUp();
    if (stage >= 5) {
        const wwlfw_ops ops = {msaAssign, boardData, nullptr, FW_WLAN_MSA_PHYS, static_cast<uint32_t>(FW_WLAN_MSA_SIZE), 1};
        gWlfwOn = wwlfw_init(&gWlfw, &gQrtr, &ops) == 0;
    }
    fajita_wifi_state[0] = stage;
    uint64_t last = 0;
    for (;;) {
        IOLockLock(gInitLock);
        if (gSmp2pUp) wsmp2p_irq(&gSmp2p);
        IOLockUnlock(gInitLock);
        const int g = wglink_rx(&gGlink);
        wqrtr_flush(&gQrtr);
        if (stage >= 4) wtftp_poll(&gTftp);
        if (gWlfwOn) {
            wwlfw_poll(&gWlfw, nowMs());
            if (gWlfw.state == WWLFW_FW_READY && !gEnableSent) {gEnableSent = true; wwlfw_enable(&gWlfw, WLFW_MODE_MISSION);}
        }
        const uint64_t t = nowMs();
        if (t - last >= 1000) {
            last = t;
            char svcs[64] = "";
            for (unsigned i = 0; i < gNSeen && strlen(svcs) < 56; ++i)
                snprintf(svcs + strlen(svcs), sizeof svcs - strlen(svcs), "%s%x", i ? "," : "", gSeen[i].service);
            uint32_t bits = 0;
            if (gSmp2p.in) {uint32_t ch; wsmp2p_take(&gSmp2p, "slave-kernel", &bits, &ch);}
            row(4, "FW3 smp2p %x glink v%s fifo1 %s rx%u tx%u err%u ipcrtr %s qrtr hello %d rx%u bad%u servers %s%s",
                bits, gGlink.version_acked ? "1" : "?", gGlink.rx.fifo ? "yes" : "no", gGlink.rx_cmds, gGlink.tx_cmds,
                gGlink.errors, wglink_ready(&gGlink, gIpcrtr) ? "open" : "closed", gQrtr.hello_seen, gQrtr.rx_pkts,
                gQrtr.bad, svcs[0] ? svcs : "-", g < 0 ? " GLINK-FAULT" : "");
            if (stage >= 4)
                row(5, "FW4 pdr %u tftp rrq%u wrq%u %llukB err%u refused%u last %.40s rmtfs open%u iov%u sect%u wr-refused%u",
                    gPdm.requests, gTftp.rrq, gTftp.wrq, gTftp.bytes >> 10, gTftp.errors, gTftp.refused, gTftp.last,
                    gRmtfs.opens, gRmtfs.iovecs, gRmtfs.sectors_read, gRmtfs.writes_refused);
            if (gWlfwOn)
                row(6, "FW5 wlfw %s%s%s ind%u cap chip=%x board=%x fw=%x msa%u%s bdf %s %u/%u seg%u res%u/%u fw_ready%d",
                    wwlfw_state_name(gWlfw.state), gWlfw.state == WWLFW_FAILED ? " in " : "",
                    gWlfw.state == WWLFW_FAILED ? wwlfw_state_name(gWlfw.failed_in) : "", gWlfw.indications,
                    gWlfw.cap.chip_id, gWlfw.cap.board_id, gWlfw.cap.fw_version, gWlfw.msa.count,
                    gWlfw.failed_in == WWLFW_MSA_DELAY && gMsaRc == -78 ? " msa-assign?" : gWlfw.msa_assigned ? " assigned" : "",
                    gBoardName[0] ? gBoardName + 6 : "-", gWlfw.bd_off, gWlfw.bd_len, gWlfw.bd_seg, gWlfw.last_result,
                    gWlfw.last_error, gWlfw.fw_ready);
        }
        if (g < 0) IOSleep(100);   // a GLINK protocol fault: keep the rows, do not spin
        else IOSleep(2);
    }
}
