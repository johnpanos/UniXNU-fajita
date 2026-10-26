// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita network interface: a BSD ifnet "en0" (IFT_ETHER, family ETHERNET) that iOS's network stack, configd's
// IPConfiguration agent (ipconfig / DHCP), routing and the sockets above treat as an Ethernet interface, with a pluggable
// frame backend. `fajita-net=usb` uses the USB network channel (drivers/NativeDwc3/UsbNet.h: fajita_usbnet_send and
// fajita_usbnet_register_rx, `usb-net=1`) so DHCP and ping can be proven against the laptop (scripts/fajita/test/net-test.sh,
// passt) before the Wi-Fi radio carries frames; the Wi-Fi data path will call fajita_net_input and fajita_net_set_backend.
//
// Why an ifnet and not an IOEthernetController: this XNU is built from source, so the BSD KPI (net/kpi_interface.h, the same
// calls bsd/net/if_fake.c's feth uses) is directly available, while a subclass of IOEthernetController needs the
// IONetworkingFamily vtables of the stripped 20H392 kernelcache reconstructed (see NOTES-wifi.md, "iOS interface"). IPConfiguration
// builds its interface list from the kernel's ifnet list, so `ipconfig set en0 DHCP` works for this interface.
//
// Frames: Ethernet II, 14..1536 bytes, no FCS. Receive: the backend's callback copies into a ring (it may run on a poll thread
// that must not block); the interface's own thread turns each frame into an mbuf and calls ifnet_input. Transmit: the ifnet start
// routine dequeues, flattens each packet and calls the backend; a full backend ring drops the packet (counted).
#include <libkern/c++/OSMetaClass.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/kpi_mbuf.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/if_media.h>
#include <net/ethernet.h>
#include <net/kpi_interface.h>
extern "C" {
#include <net/bpf.h>
}

extern "C" {
int fajita_usbnet_send(const void *frame, size_t len) __attribute__((weak));
int fajita_usbnet_register_rx(void (*fn)(void *ctx, const void *frame, size_t len), void *ctx) __attribute__((weak));
int fajita_usbnet_link_up(void) __attribute__((weak));
kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help) __attribute__((weak));
errno_t ether_demux(ifnet_t, mbuf_t, char *, protocol_family_t *);
errno_t ether_add_proto(ifnet_t, protocol_family_t, const struct ifnet_demux_desc *, u_int32_t);
errno_t ether_del_proto(ifnet_t, protocol_family_t);
errno_t ether_check_multi(ifnet_t, const struct sockaddr *);
errno_t ether_frameout_extended(ifnet_t, mbuf_t *, const struct sockaddr *, const char *, const char *, u_int32_t *, u_int32_t *);
extern u_char etherbroadcastaddr[ETHER_ADDR_LEN];
int fajita_net_start(void);
int fajita_net_input(const void *frame, size_t len);
void fajita_net_set_backend(int (*send)(void *ctx, const void *frame, size_t len), void *ctx);
// [0] ifnet attached, [1] frames in, [2] frames out, [3] dropped in, [4] dropped out, [5] backend errors
volatile uint32_t fajita_net_state[8];
}

namespace {
constexpr unsigned kRing = 64, kFrame = 1536;
struct Slot {uint16_t len; uint8_t data[kFrame];};
Slot gRing[kRing];
volatile unsigned gHead, gTail;      // producer (backend callback) / consumer (the interface's thread)
IOSimpleLock *gRingLock;
ifnet_t gIf;
uint8_t gMac[ETHER_ADDR_LEN] = {0x02, 0xfa, 0x6a, 0x17, 0x4e, 0x02};
int (*gSend)(void *, const void *, size_t);
void *gSendCtx;
bool gStarted;

int usbSend(void *, const void *frame, size_t len) {return fajita_usbnet_send ? fajita_usbnet_send(frame, len) : -1;}

void ringPush(const void *frame, size_t len)
{
    if (len < ETHER_HDR_LEN || len > kFrame) {fajita_net_state[3] = fajita_net_state[3] + 1; return;}
    IOInterruptState st = IOSimpleLockLockDisableInterrupt(gRingLock);
    const unsigned next = (gHead + 1) % kRing;
    if (next == gTail) {IOSimpleLockUnlockEnableInterrupt(gRingLock, st); fajita_net_state[3] = fajita_net_state[3] + 1; return;}
    gRing[gHead].len = uint16_t(len);
    memcpy(gRing[gHead].data, frame, len);
    gHead = next;
    IOSimpleLockUnlockEnableInterrupt(gRingLock, st);
}
void usbRx(void *, const void *frame, size_t len) {ringPush(frame, len);}

// A frame from the backend -> an mbuf for ifnet_input, as if_fake's feth_output_common does for its peer.
void deliver(const uint8_t *frame, size_t len)
{
    mbuf_t m = nullptr;
    unsigned chunks = 1;
    if (mbuf_allocpacket(MBUF_DONTWAIT, len, &chunks, &m) || !m) {fajita_net_state[3] = fajita_net_state[3] + 1; return;}
    if (mbuf_copyback(m, 0, len, frame, MBUF_DONTWAIT)) {mbuf_freem(m); fajita_net_state[3] = fajita_net_state[3] + 1; return;}
    mbuf_setlen(m, len);
    mbuf_pkthdr_setlen(m, len);
    char *header = static_cast<char *>(mbuf_data(m));
    mbuf_pkthdr_setrcvif(m, gIf);
    mbuf_pkthdr_setheader(m, header);
    mbuf_pkthdr_adjustlen(m, -int(ETHER_HDR_LEN));
    mbuf_setdata(m, header + ETHER_HDR_LEN, len - ETHER_HDR_LEN);
    bpf_tap_in(gIf, DLT_EN10MB, m, header, ETHER_HDR_LEN);
    struct ifnet_stat_increment_param stats = {};
    stats.packets_in = 1;
    stats.bytes_in = uint32_t(len);
    ifnet_input(gIf, m, &stats);
    fajita_net_state[1] = fajita_net_state[1] + 1;
}

void rxThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "fajita-net");
    for (;;) {
        bool any = false;
        for (;;) {
            Slot *slot = nullptr;
            static Slot copy;
            IOInterruptState st = IOSimpleLockLockDisableInterrupt(gRingLock);
            if (gTail != gHead) {copy.len = gRing[gTail].len; memcpy(copy.data, gRing[gTail].data, copy.len); slot = &copy; gTail = (gTail + 1) % kRing;}
            IOSimpleLockUnlockEnableInterrupt(gRingLock, st);
            if (!slot) break;
            any = true;
            if (gIf) deliver(slot->data, slot->len);
        }
        if (!any) IOSleep(1);
    }
}

// ifnet_start_func: the stack queued packets; send them.
void startFn(ifnet_t ifp)
{
    mbuf_t m;
    static uint8_t buf[kFrame];
    while (ifnet_dequeue(ifp, &m) == 0) {
        const size_t len = mbuf_pkthdr_len(m);
        if (len < ETHER_HDR_LEN || len > kFrame || mbuf_copydata(m, 0, len, buf)) {
            ifnet_stat_increment_out(ifp, 0, 0, 1);
            fajita_net_state[4] = fajita_net_state[4] + 1;
            mbuf_freem(m);
            continue;
        }
        bpf_tap_out(ifp, DLT_EN10MB, m, nullptr, 0);
        const int rc = gSend ? gSend(gSendCtx, buf, len) : -1;
        if (rc) {
            ifnet_stat_increment_out(ifp, 0, 0, 1);
            fajita_net_state[rc == -3 ? 4 : 5] = fajita_net_state[rc == -3 ? 4 : 5] + 1;
        } else {
            ifnet_stat_increment_out(ifp, 1, uint32_t(len), 0);
            fajita_net_state[2] = fajita_net_state[2] + 1;
        }
        mbuf_freem(m);
    }
}

errno_t ioctlFn(ifnet_t ifp, u_long cmd, void *data)
{
    ifreq *ifr = static_cast<ifreq *>(data);
    switch (cmd) {
    case SIOCSIFADDR:
        ifnet_set_flags(ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
        return 0;
    case SIOCSIFFLAGS:
        if (ifnet_flags(ifp) & IFF_UP) ifnet_set_flags(ifp, IFF_RUNNING, IFF_RUNNING);
        else ifnet_set_flags(ifp, 0, IFF_RUNNING);
        return 0;
    case SIOCADDMULTI:
    case SIOCDELMULTI:
        return 0;
    case SIOCGIFDEVMTU:
        ifr->ifr_devmtu.ifdm_current = ifnet_mtu(ifp);
        ifr->ifr_devmtu.ifdm_max = 1500;
        ifr->ifr_devmtu.ifdm_min = IF_MINMTU;
        return 0;
    case SIOCSIFMTU:
        if (ifr->ifr_mtu > 1500 || ifr->ifr_mtu < IF_MINMTU) return EINVAL;
        return ifnet_set_mtu(ifp, ifr->ifr_mtu);
    case SIOCSIFLLADDR:
        return ifnet_set_lladdr(ifp, ifr->ifr_addr.sa_data, ifr->ifr_addr.sa_len);
    case SIOCGIFMEDIA32:
    case SIOCGIFMEDIA64: {   // one 100baseTX full-duplex "wire"; active while the backend has a link
        ifmediareq *ifmr = static_cast<ifmediareq *>(data);
        ifmr->ifm_active = ifmr->ifm_current = IFM_ETHER | IFM_100_TX | IFM_FDX;
        ifmr->ifm_mask = 0;
        ifmr->ifm_status = IFM_AVALID | (gSend ? IFM_ACTIVE : 0);
        ifmr->ifm_count = 0;
        return 0;
    }
    default:
        return EOPNOTSUPP;
    }
}

size_t kcmdNet(const char *, size_t, char *out, size_t cap)
{
    const int n = snprintf(out, cap, "net: ifnet %s in %u out %u drop-in %u drop-out %u backend-err %u backend %s\n", fajita_net_state[0] ? "en0 attached" : "absent",
                           fajita_net_state[1], fajita_net_state[2], fajita_net_state[3], fajita_net_state[4], fajita_net_state[5], gSend ? "up" : "none");
    return n > 0 ? (size_t(n) < cap ? size_t(n) : cap) : 0;
}

void detachFn(ifnet_t ifp) {(void)ifp; gIf = nullptr;}

bool attach()
{
    ifnet_init_eparams init;
    bzero(&init, sizeof init);
    init.ver = IFNET_INIT_CURRENT_VERSION;
    init.len = sizeof init;
    init.uniqueid = "fajita-en0";
    init.uniqueid_len = 10;
    init.name = "en";
    init.unit = 0;
    init.family = IFNET_FAMILY_ETHERNET;
    init.type = IFT_ETHER;
    init.output_sched_model = IFNET_SCHED_MODEL_NORMAL;
    init.start = startFn;
    init.demux = ether_demux;
    init.add_proto = ether_add_proto;
    init.del_proto = ether_del_proto;
    init.check_multi = ether_check_multi;
    init.framer_extended = ether_frameout_extended;
    init.softc = nullptr;
    init.ioctl = ioctlFn;
    init.detach = detachFn;
    init.broadcast_addr = etherbroadcastaddr;
    init.broadcast_len = ETHER_ADDR_LEN;
    ifnet_t ifp = nullptr;
    if (ifnet_allocate_extended(&init, &ifp) || !ifp) return false;
    ifnet_set_mtu(ifp, 1500);
    ifnet_set_addrlen(ifp, ETHER_ADDR_LEN);
    ifnet_set_hdrlen(ifp, ETHER_HDR_LEN);
    ifnet_set_baudrate(ifp, 100000000ull);
    ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
    if (ifnet_attach(ifp, nullptr)) {ifnet_release(ifp); return false;}
    ifnet_set_lladdr(ifp, gMac, sizeof gMac);
    bpfattach(ifp, DLT_EN10MB, ETHER_HDR_LEN);
    gIf = ifp;
    fajita_net_state[0] = 1;
    return true;
}

void worker(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "fajita-net-init");
    char mode[8] = {};
    PE_parse_boot_argn("fajita-net", mode, sizeof mode);
    if (!strcmp(mode, "usb")) {
        // the USB channel needs a configured gadget: wait for the host side to come up
        for (unsigned i = 0; i < 600 && !(fajita_usbnet_link_up && fajita_usbnet_link_up()); ++i) IOSleep(500);
        if (!fajita_usbnet_register_rx) {IOLog("fajita-net: no USB network channel linked\n"); return;}
        gSend = usbSend;
        fajita_usbnet_register_rx(usbRx, nullptr);
    }
    if (!attach()) {IOLog("fajita-net: ifnet attach failed\n"); return;}
    if (fajita_usbdbg_register_kcmd) fajita_usbdbg_register_kcmd("net", kcmdNet, "fajita-net interface counters");
    IOLog("fajita-net: en0 attached (%s), mac %02x:%02x:%02x:%02x:%02x:%02x\n", mode, gMac[0], gMac[1], gMac[2], gMac[3], gMac[4], gMac[5]);
    thread_t t = nullptr;
    if (kernel_thread_start(rxThread, nullptr, &t) == KERN_SUCCESS) thread_deallocate(t);
}
}  // namespace

// Called once from the boot progress monitor (fajita_wifi_poll) when `fajita-net` is given. 0 if started.
extern "C" int fajita_net_start(void)
{
    if (gStarted) return 0;
    gStarted = true;
    if (!gRingLock) gRingLock = IOSimpleLockAlloc();
    thread_t t = nullptr;
    if (kernel_thread_start(worker, nullptr, &t) != KERN_SUCCESS) return -1;
    thread_deallocate(t);
    return 0;
}

// The Wi-Fi data path (or any backend) hands a received Ethernet frame to the interface; any thread, never blocks.
extern "C" int fajita_net_input(const void *frame, size_t len)
{
    if (!gRingLock) return -1;
    ringPush(frame, len);
    return 0;
}

extern "C" void fajita_net_set_backend(int (*send)(void *ctx, const void *frame, size_t len), void *ctx)
{
    gSendCtx = ctx;
    gSend = send;
}
