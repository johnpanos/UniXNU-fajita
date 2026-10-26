// SPDX-License-Identifier: BSD-2-Clause-Patent
// fjnet: the few network commands iOS does not ship (no ifconfig/ipconfig/ping binaries on the device), for proving a network
// interface from the USB debug channel. Built by scripts/fajita/build-usbdbg-agent.sh next to fjdbg-agent.
//   fjnet show IF           address, netmask, flags, MTU, MAC, counters of IF
//   fjnet dhcp IF           DHCP DISCOVER/REQUEST on IF, then address + netmask + default route; prints "lease A.B.C.D gw G.G.G.G"
//   fjnet sc [REGEX]        what configd's SCDynamicStore knows: keys matching REGEX (default Network/Interface) and their values
//   fjnet http URL          a GET through CFNetwork (the stack apps use): status code, bytes, first line
//   fjnet ping IP [COUNT]   ICMP echo through the normal routing table; prints "N/M received"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <CFNetwork/CFNetwork.h>
#include <dlfcn.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static int show(const char *ifn)
{
    struct ifaddrs *l, *a;
    if (getifaddrs(&l)) {perror("getifaddrs"); return 1;}
    int found = 0;
    for (a = l; a; a = a->ifa_next) {
        if (strcmp(a->ifa_name, ifn) || !a->ifa_addr) continue;
        found = 1;
        if (a->ifa_addr->sa_family == AF_INET) {
            char ip[32] = "", nm[32] = "";
            inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip, sizeof ip);
            if (a->ifa_netmask) inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_netmask)->sin_addr, nm, sizeof nm);
            printf("%s: inet %s netmask %s flags 0x%x\n", ifn, ip, nm, a->ifa_flags);
        } else if (a->ifa_addr->sa_family == AF_LINK) {
            struct sockaddr_dl *d = (void *)a->ifa_addr;
            const unsigned char *m = (const unsigned char *)LLADDR(d);
            printf("%s: link %02x:%02x:%02x:%02x:%02x:%02x flags 0x%x", ifn, m[0], m[1], m[2], m[3], m[4], m[5], a->ifa_flags);
            if (a->ifa_data) {
                const struct if_data *s = a->ifa_data;
                printf(" mtu %u ipkts %u opkts %u ierrs %u oerrs %u", s->ifi_mtu, s->ifi_ipackets, s->ifi_opackets, s->ifi_ierrors, s->ifi_oerrors);
            }
            printf("\n");
        }
    }
    freeifaddrs(l);
    if (!found) printf("%s: no such interface\n", ifn);
    return found ? 0 : 1;
}

static int set_addr(const char *ifn, struct in_addr ip, struct in_addr mask)
{
    struct ifaliasreq r;
    memset(&r, 0, sizeof r);
    strlcpy(r.ifra_name, ifn, sizeof r.ifra_name);
    struct sockaddr_in *s = (void *)&r.ifra_addr;
    s->sin_family = AF_INET; s->sin_len = sizeof *s; s->sin_addr = ip;
    s = (void *)&r.ifra_mask;
    s->sin_family = AF_INET; s->sin_len = sizeof *s; s->sin_addr = mask;
    s = (void *)&r.ifra_broadaddr;                  // limited broadcasts need the interface to have a broadcast address
    s->sin_family = AF_INET; s->sin_len = sizeof *s; s->sin_addr.s_addr = ip.s_addr | ~mask.s_addr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int rc = ioctl(fd, SIOCAIFADDR, &r);
    if (rc) perror("SIOCAIFADDR");
    close(fd);
    return rc;
}

// route(4) RTM_ADD default via gw (the kernel's rt_msghdr is 92 bytes; the iOS SDK has no net/route.h)
static int add_default(struct in_addr gw)
{
    uint8_t m[92 + 3 * 16];
    memset(m, 0, sizeof m);
    const uint16_t len = sizeof m;
    memcpy(m, &len, 2);
    m[2] = 5; m[3] = 1;                              // RTM_VERSION, RTM_ADD
    const int flags = 0x1 | 0x2 | 0x800, addrs = 1 | 2 | 4, seq = 1, pid = getpid();
    memcpy(m + 8, &flags, 4); memcpy(m + 12, &addrs, 4); memcpy(m + 16, &pid, 4); memcpy(m + 20, &seq, 4);
    for (int i = 0; i < 3; i++) {                    // dst 0.0.0.0, gateway, mask 0.0.0.0
        struct sockaddr_in s;
        memset(&s, 0, sizeof s);
        s.sin_len = sizeof s; s.sin_family = AF_INET;
        if (i == 1) s.sin_addr = gw;
        memcpy(m + 92 + 16 * i, &s, 16);
    }
    int fd = socket(PF_ROUTE, SOCK_RAW, 0);
    if (fd < 0) {perror("PF_ROUTE"); return -1;}
    int rc = write(fd, m, sizeof m) == (ssize_t)sizeof m ? 0 : -1;
    if (rc) perror("RTM_ADD");
    close(fd);
    return rc;
}

#define DHCP_MAGIC 0x63825363u
static int dhcp_msg(uint8_t *p, uint8_t type, const uint8_t mac[6], uint32_t xid, struct in_addr req, struct in_addr srv)
{
    memset(p, 0, 300);
    p[0] = 1; p[1] = 1; p[2] = 6;
    memcpy(p + 4, &xid, 4);
    p[10] = 0x80;                                     // broadcast flag: no address yet to unicast to
    memcpy(p + 28, mac, 6);
    const uint32_t mg = htonl(DHCP_MAGIC);
    memcpy(p + 236, &mg, 4);
    uint8_t *o = p + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    if (type == 3) {
        *o++ = 50; *o++ = 4; memcpy(o, &req, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, &srv, 4); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;
    *o++ = 255;
    return 300;
}

static uint16_t cksum(const void *p, int n);
// net/bpf.h is not in the iPhoneOS SDK: the pieces used here, from XNU's bsd/net/bpf.h (user space: 32-bit time stamps)
struct fj_timeval32 {int32_t tv_sec, tv_usec;};
struct bpf_hdr {struct fj_timeval32 bh_tstamp; uint32_t bh_caplen, bh_datalen; unsigned short bh_hdrlen;};
#define BPF_WORDALIGN(x) (((x) + 3) & ~3)
#define BIOCGBLEN _IOR('B', 102, unsigned)
#define BIOCSETIF _IOW('B', 108, struct ifreq)
#define BIOCIMMEDIATE _IOW('B', 112, unsigned)
#define BIOCSHDRCMPLT _IOW('B', 117, unsigned)
// DHCP runs on raw frames through BPF, as configd's IPConfiguration does: DISCOVER/REQUEST from 0.0.0.0:68 to
// 255.255.255.255:67 (a real server ignores anything else: the earlier UDP-socket version sent from a link-local address to
// the link-local broadcast, which only passt answered), replies read back on UDP port 68.
static int bpf_open(const char *ifn)
{
    for (int i = 0; i < 16; i++) {
        char path[16];
        snprintf(path, sizeof path, "/dev/bpf%d", i);
        int fd = open(path, O_RDWR);
        if (fd < 0) continue;
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        strlcpy(ifr.ifr_name, ifn, sizeof ifr.ifr_name);
        unsigned one = 1;
        if (ioctl(fd, BIOCSETIF, &ifr) || ioctl(fd, BIOCIMMEDIATE, &one) || ioctl(fd, BIOCSHDRCMPLT, &one)) {close(fd); continue;}
        return fd;
    }
    return -1;
}
static int dhcp_frame(uint8_t *f, const uint8_t mac[6], const uint8_t *msg, int mlen)
{
    memset(f, 0xff, 6); memcpy(f + 6, mac, 6); f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14, *udp = ip + 20;
    memset(ip, 0, 20);
    ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
    const int iplen = 20 + 8 + mlen;
    ip[2] = (uint8_t)(iplen >> 8); ip[3] = (uint8_t)iplen;
    memset(ip + 16, 0xff, 4);                         // to 255.255.255.255, from 0.0.0.0
    const uint16_t c = cksum(ip, 20);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    udp[0] = 0; udp[1] = 68; udp[2] = 0; udp[3] = 67;
    udp[4] = (uint8_t)((8 + mlen) >> 8); udp[5] = (uint8_t)(8 + mlen); udp[6] = udp[7] = 0;   // no UDP checksum (allowed for IPv4)
    memcpy(udp + 8, msg, mlen);
    return 14 + iplen;
}
// the next DHCP reply for xid within ms milliseconds: its BOOTP payload into out, or -1
static int dhcp_recv(int fd, uint32_t xid, uint8_t *out, int cap, int ms)
{
    unsigned blen = 4096;
    ioctl(fd, BIOCGBLEN, &blen);
    uint8_t *buf = malloc(blen);
    if (!buf) return -1;
    struct timeval t0, t;
    gettimeofday(&t0, NULL);
    int got = -1;
    for (;;) {
        gettimeofday(&t, NULL);
        const int left = ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_usec - t0.tv_usec) / 1000);
        if (left <= 0) break;
        struct pollfd pf = {fd, POLLIN, 0};
        if (poll(&pf, 1, left) <= 0) break;
        ssize_t n = read(fd, buf, blen);
        for (uint8_t *p = buf; n > 0 && p < buf + n && got < 0;) {
            const struct bpf_hdr *h = (const struct bpf_hdr *)p;
            const uint8_t *e = p + h->bh_hdrlen;
            const unsigned cl = h->bh_caplen;
            if (cl >= 14 + 20 + 8 + 240 && e[12] == 0x08 && e[13] == 0x00 && e[14 + 9] == 17) {
                const unsigned ihl = (e[14] & 15u) * 4u;
                const uint8_t *u = e + 14 + ihl;
                if (u + 8 <= e + cl && u[2] == 0 && u[3] == 68) {
                    const int pl = (int)(e + cl - (u + 8));
                    if (pl >= 240 && !memcmp(u + 8 + 4, &xid, 4) && u[8] == 2) {got = pl < cap ? pl : cap; memcpy(out, u + 8, got);}
                }
            }
            p += BPF_WORDALIGN(h->bh_hdrlen + h->bh_caplen);
        }
        if (got >= 0) break;
    }
    free(buf);
    return got;
}

static int dhcp(const char *ifn)
{
    struct ifaddrs *l, *a;
    uint8_t mac[6] = {0};
    if (getifaddrs(&l)) return 1;
    for (a = l; a; a = a->ifa_next)
        if (!strcmp(a->ifa_name, ifn) && a->ifa_addr && a->ifa_addr->sa_family == AF_LINK)
            memcpy(mac, LLADDR((struct sockaddr_dl *)a->ifa_addr), 6);
    freeifaddrs(l);
    if (!if_nametoindex(ifn)) {fprintf(stderr, "no interface %s\n", ifn); return 1;}
    {   // the interface must be up for BPF to send
        struct ifreq ifr;
        memset(&ifr, 0, sizeof ifr);
        strlcpy(ifr.ifr_name, ifn, sizeof ifr.ifr_name);
        int c = socket(AF_INET, SOCK_DGRAM, 0);
        if (!ioctl(c, SIOCGIFFLAGS, &ifr)) {ifr.ifr_flags |= IFF_UP; ioctl(c, SIOCSIFFLAGS, &ifr);}
        close(c);
    }
    int fd = bpf_open(ifn);
    if (fd < 0) {fprintf(stderr, "dhcp: no /dev/bpf for %s (%s)\n", ifn, strerror(errno)); return 1;}
    const uint32_t xid = (uint32_t)getpid() * 2654435761u;
    uint8_t tx[300], fr[400], rx[600];
    struct in_addr offer = {0}, srv = {0}, mask = {0}, gw = {0};
    for (int stage = 1; stage <= 2; stage++) {
        int got = 0;
        for (int tries = 0; tries < 5 && !got; tries++) {
            const int n = dhcp_msg(tx, stage == 1 ? 1 : 3, mac, xid, offer, srv);
            const int fl = dhcp_frame(fr, mac, tx, n);
            if (write(fd, fr, fl) != fl) perror("bpf write");
            const int r = dhcp_recv(fd, xid, rx, sizeof rx, 3000);
            if (r < 240) continue;
            uint8_t type = 0;
            struct in_addr m = {0}, g = {0}, s = {0};
            for (uint8_t *o = rx + 240; o < rx + r && *o != 255;) {
                if (!*o) {o++; continue;}
                if (o + 2 > rx + r || o + 2 + o[1] > rx + r) break;
                if (o[0] == 53) type = o[2];
                else if (o[0] == 1 && o[1] == 4) memcpy(&m, o + 2, 4);
                else if (o[0] == 3 && o[1] >= 4) memcpy(&g, o + 2, 4);
                else if (o[0] == 54 && o[1] == 4) memcpy(&s, o + 2, 4);
                o += 2 + o[1];
            }
            if (stage == 1 && type == 2) {memcpy(&offer, rx + 16, 4); srv = s; mask = m; gw = g; got = 1;}
            if (stage == 2 && type == 5) {memcpy(&offer, rx + 16, 4); if (m.s_addr) mask = m; if (g.s_addr) gw = g; got = 1;}
            if (stage == 2 && type == 6) {fprintf(stderr, "NAK\n"); close(fd); return 1;}
        }
        if (!got) {fprintf(stderr, "dhcp: no %s\n", stage == 1 ? "OFFER" : "ACK"); close(fd); return 1;}
    }
    close(fd);
    if (!mask.s_addr) mask.s_addr = htonl(0xffffff00u);
    if (set_addr(ifn, offer, mask)) return 1;
    if (gw.s_addr && add_default(gw)) fprintf(stderr, "default route failed\n");
    char a1[32], a2[32];
    inet_ntop(AF_INET, &offer, a1, sizeof a1);
    inet_ntop(AF_INET, &gw, a2, sizeof a2);
    printf("lease %s gw %s\n", a1, a2);
    return 0;
}

static uint16_t cksum(const void *p, int n)
{
    const uint8_t *b = p;
    uint32_t s = 0;
    for (; n > 1; n -= 2, b += 2) s += (uint32_t)(b[0] << 8 | b[1]);
    if (n) s += (uint32_t)b[0] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

static int ping(const char *ip, int count)
{
    struct sockaddr_in d = {.sin_len = sizeof d, .sin_family = AF_INET};
    if (inet_pton(AF_INET, ip, &d.sin_addr) != 1) {fprintf(stderr, "bad address\n"); return 1;}
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) {perror("icmp socket"); return 1;}
    int ok = 0;
    for (int i = 0; i < count; i++) {
        uint8_t p[40];
        memset(p, 0, sizeof p);
        p[0] = 8; p[6] = 0; p[7] = (uint8_t)(i + 1);
        for (int k = 8; k < 40; k++) p[k] = (uint8_t)k;
        const uint16_t c = cksum(p, sizeof p);
        p[2] = (uint8_t)(c >> 8); p[3] = (uint8_t)c;
        struct timeval t0, t1;
        gettimeofday(&t0, NULL);
        if (sendto(fd, p, sizeof p, 0, (void *)&d, sizeof d) < 0) {perror("sendto"); continue;}
        for (;;) {
            struct pollfd pf = {fd, POLLIN, 0};
            gettimeofday(&t1, NULL);
            const int el = (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000);
            if (el >= 3000 || poll(&pf, 1, 3000 - el) <= 0) {printf("seq %d timeout\n", i + 1); break;}
            uint8_t r[256];
            ssize_t n = recv(fd, r, sizeof r, 0);
            const int h = n > 20 && (r[0] >> 4) == 4 ? (r[0] & 15) * 4 : 0;   // a DGRAM ICMP socket may include the IP header
            if (n - h >= 8 && r[h] == 0) {
                gettimeofday(&t1, NULL);
                printf("seq %d reply %ld ms\n", i + 1, (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000));
                ok++;
                break;
            }
        }
        usleep(300000);
    }
    printf("%d/%d received\n", ok, count);
    return ok ? 0 : 1;
}

static int sc(const char *re)
{
    // the iOS SDK marks SCDynamicStore unavailable although configd serves it: resolve at run time
    void *h = dlopen("/System/Library/Frameworks/SystemConfiguration.framework/SystemConfiguration", RTLD_NOW);
    CFTypeRef (*SCCreate)(CFAllocatorRef, CFStringRef, void *, void *) = h ? dlsym(h, "SCDynamicStoreCreate") : NULL;
    CFArrayRef (*SCKeys)(CFTypeRef, CFStringRef) = h ? dlsym(h, "SCDynamicStoreCopyKeyList") : NULL;
    CFPropertyListRef (*SCValue)(CFTypeRef, CFStringRef) = h ? dlsym(h, "SCDynamicStoreCopyValue") : NULL;
    if (!SCCreate || !SCKeys || !SCValue) {printf("SystemConfiguration symbols missing\n"); return 1;}
    CFTypeRef st = SCCreate(NULL, CFSTR("fjnet"), NULL, NULL);
    if (!st) {printf("SCDynamicStoreCreate failed\n"); return 1;}
    CFStringRef pat = CFStringCreateWithCString(NULL, re, kCFStringEncodingUTF8);
    CFArrayRef keys = SCKeys(st, pat);
    if (!keys) {printf("no keys match %s\n", re); return 1;}
    printf("%ld keys match %s\n", (long)CFArrayGetCount(keys), re);
    for (CFIndex i = 0; i < CFArrayGetCount(keys); i++) {
        CFStringRef k = CFArrayGetValueAtIndex(keys, i);
        CFPropertyListRef v = SCValue(st, k);
        char kb[256], vb[1024] = "";
        CFStringGetCString(k, kb, sizeof kb, kCFStringEncodingUTF8);
        if (v) {
            CFStringRef d = CFCopyDescription(v);
            CFStringGetCString(d, vb, sizeof vb, kCFStringEncodingUTF8);
            CFRelease(d); CFRelease(v);
            for (char *c = vb; *c; c++) if (*c == '\n' || *c == '\t') *c = ' ';
        }
        printf("%s = %s\n", kb, vb);
    }
    return 0;
}

static int http(const char *url)
{
    CFStringRef us = CFStringCreateWithCString(NULL, url, kCFStringEncodingUTF8);
    CFURLRef u = CFURLCreateWithString(NULL, us, NULL);
    CFHTTPMessageRef req = CFHTTPMessageCreateRequest(NULL, CFSTR("GET"), u, kCFHTTPVersion1_1);
    CFReadStreamRef rs = CFReadStreamCreateForHTTPRequest(NULL, req);
    if (!CFReadStreamOpen(rs)) {printf("open failed\n"); return 1;}
    uint8_t buf[2048];
    long total = 0;
    char first[120] = "";
    time_t t0 = time(NULL);
    for (;;) {
        CFIndex n = CFReadStreamRead(rs, buf, sizeof buf);
        if (n > 0) {if (!total) {size_t m = n < 100 ? (size_t)n : 100; memcpy(first, buf, m); first[m] = 0;} total += n;}
        else break;
        if (time(NULL) - t0 > 20) break;
    }
    CFHTTPMessageRef resp = (CFHTTPMessageRef)CFReadStreamCopyProperty(rs, kCFStreamPropertyHTTPResponseHeader);
    long code = resp ? (long)CFHTTPMessageGetResponseStatusCode(resp) : -1;
    CFErrorRef err = CFReadStreamCopyError(rs);
    printf("http %ld bytes %ld", code, total);
    if (err) {printf(" error %ld domain %s", (long)CFErrorGetCode(err), CFStringGetCStringPtr(CFErrorGetDomain(err), kCFStringEncodingUTF8));}
    for (char *c = first; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
    printf(" first '%s'\n", first);
    return code == 200 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "show")) return show(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "dhcp")) return dhcp(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "sc")) return sc(argc > 2 ? argv[2] : "State:/Network/(Interface|Global).*");
    if (argc >= 3 && !strcmp(argv[1], "http")) return http(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "ping")) return ping(argv[2], argc > 3 ? atoi(argv[3]) : 3);
    fprintf(stderr, "usage: fjnet show IF | dhcp IF | sc [REGEX] | http URL | ping IP [COUNT]\n");
    return 2;
}
