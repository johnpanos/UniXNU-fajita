// SPDX-License-Identifier: BSD-2-Clause-Patent
// fjnet: the few network commands iOS does not ship (no ifconfig/ipconfig/ping binaries on the device), for proving a network
// interface from the USB debug channel. Built by scripts/fajita/build-usbdbg-agent.sh next to fjdbg-agent.
//   fjnet show IF           address, netmask, flags, MTU, MAC, counters of IF
//   fjnet dhcp IF           DHCP DISCOVER/REQUEST on IF, then address + netmask + default route; prints "lease A.B.C.D gw G.G.G.G"
//   fjnet ping IP [COUNT]   ICMP echo through the normal routing table; prints "N/M received"
#include <arpa/inet.h>
#include <errno.h>
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

static int dhcp(const char *ifn)
{
    struct ifaddrs *l, *a;
    uint8_t mac[6] = {0};
    if (getifaddrs(&l)) return 1;
    for (a = l; a; a = a->ifa_next)
        if (!strcmp(a->ifa_name, ifn) && a->ifa_addr && a->ifa_addr->sa_family == AF_LINK)
            memcpy(mac, LLADDR((struct sockaddr_dl *)a->ifa_addr), 6);
    freeifaddrs(l);
    const unsigned idx = if_nametoindex(ifn);
    if (!idx) {fprintf(stderr, "no interface %s\n", ifn); return 1;}
    // the interface needs an address (0.0.0.0 is refused) before sockets can send out of it: use link-local until the lease
    struct in_addr tmp = {htonl(0xa9fe0001u)}, m16 = {htonl(0xffff0000u)};
    set_addr(ifn, tmp, m16);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, IPPROTO_IP, 25 /* IP_BOUND_IF */, &idx, sizeof idx);
    struct sockaddr_in me = {.sin_len = sizeof me, .sin_family = AF_INET, .sin_port = htons(68)};
    if (bind(fd, (void *)&me, sizeof me)) perror("bind 68");
    struct sockaddr_in bc = {.sin_len = sizeof bc, .sin_family = AF_INET, .sin_port = htons(67), .sin_addr = {htonl(0xffffffffu)}};
    const uint32_t xid = (uint32_t)getpid() * 2654435761u;
    uint8_t tx[300], rx[600];
    struct in_addr offer = {0}, srv = {0}, mask = {0}, gw = {0}, none = {0};
    for (int stage = 1; stage <= 2; stage++) {
        int got = 0;
        for (int tries = 0; tries < 5 && !got; tries++) {
            int n = dhcp_msg(tx, stage == 1 ? 1 : 3, mac, xid, offer, srv);
            if (sendto(fd, tx, n, 0, (void *)&bc, sizeof bc) < 0) perror("sendto");
            struct pollfd pf = {fd, POLLIN, 0};
            if (poll(&pf, 1, 3000) <= 0) continue;
            ssize_t r = recv(fd, rx, sizeof rx, 0);
            if (r < 245 || memcmp(rx + 4, &xid, 4)) continue;
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
            if (stage == 2 && type == 6) {fprintf(stderr, "NAK\n"); return 1;}
        }
        if (!got) {fprintf(stderr, "dhcp: no %s\n", stage == 1 ? "OFFER" : "ACK"); return 1;}
    }
    close(fd);
    // drop the link-local placeholder, then the lease
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strlcpy(ifr.ifr_name, ifn, sizeof ifr.ifr_name);
    struct sockaddr_in *s = (void *)&ifr.ifr_addr;
    s->sin_len = sizeof *s; s->sin_family = AF_INET; s->sin_addr = tmp;
    int c = socket(AF_INET, SOCK_DGRAM, 0);
    ioctl(c, SIOCDIFADDR, &ifr);
    close(c);
    if (!mask.s_addr) mask.s_addr = htonl(0xffffff00u);
    if (set_addr(ifn, offer, mask)) return 1;
    if (gw.s_addr && add_default(gw)) fprintf(stderr, "default route failed\n");
    (void)none;
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

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "show")) return show(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "dhcp")) return dhcp(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "ping")) return ping(argv[2], argc > 3 ? atoi(argv[3]) : 3);
    fprintf(stderr, "usage: fjnet show IF | dhcp IF | ping IP [COUNT]\n");
    return 2;
}
