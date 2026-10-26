// SPDX-License-Identifier: BSD-2-Clause-Patent
// fjdbg NET channel (7): raw Ethernet frames between the phone and the laptop, one frame per fjdbg frame, both ways.
//
// This header is the transport's hardware-independent core, shared by the kernel (NativeDwc3.cpp, usb-net=1) and the
// host simulator (tests/UsbDbgSim.cpp):
//   - Net::onFrame: a frame the host sent. In echo mode it goes straight back (a pure transport test); otherwise the
//     built-in probe takes the replies addressed to it, and everything else goes to the registered receiver (the
//     future iOS network interface), or is counted as dropped.
//   - Probe: a self-contained IPv4 client used to prove the path end to end without any network interface: DHCP
//     DISCOVER -> OFFER -> REQUEST -> ACK, an ARP request for the router, then one ICMP echo to it (or to a given
//     address). Its rows go to the log; `usbnet show` prints them.
//   - Net::kcmd: the `kcmd usbnet ...` text interface (state | echo on|off | dhcp [A.B.C.D] | show).
// On the host the frames are relayed by scripts/fajita-usbdbg-hub.py (op `net`) to scripts/fajita/test/usbnet-host.py,
// which bridges them to passt (user-mode DHCP, DNS and NAT) or runs the echo test.
//
// Freestanding like UsbDbgProto.h: no allocation, no locks. Everything except the counters noted below runs on one
// thread (the kernel's USB poll thread). The includer provides snprintf.
#ifndef NATIVE_DWC3_USBNET_H
#define NATIVE_DWC3_USBNET_H

#include "UsbDbgProto.h"

namespace usbnet {

enum : size_t {
  kMaxFrame = usbdbg::kMaxData,   // 1536: a 1514-byte Ethernet frame (no FCS) fits
  kMinFrame = 14,                 // an Ethernet header
};

static inline void putBe16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static inline void putBe32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (24 - 8 * i)); }
static inline uint16_t getBe16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }
static inline uint32_t getBe32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
static inline void copy(uint8_t *d, const uint8_t *s, size_t n) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
static inline void zero(uint8_t *d, size_t n) { for (size_t i = 0; i < n; i++) d[i] = 0; }
static inline bool same(const uint8_t *a, const uint8_t *b, size_t n) { for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return false; return true; }

// RFC 1071 Internet checksum.
static inline uint16_t inetChecksum(const uint8_t *p, size_t n, uint32_t sum = 0)
{
  for (size_t i = 0; i + 1 < n; i += 2) sum += uint32_t(p[i] << 8 | p[i + 1]);
  if (n & 1) sum += uint32_t(p[n - 1] << 8);
  while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
  return uint16_t(~sum);
}

enum : uint16_t { kEthIpv4 = 0x0800, kEthArp = 0x0806 };
enum : uint8_t { kIpIcmp = 1, kIpUdp = 17 };
enum : uint16_t { kDhcpServerPort = 67, kDhcpClientPort = 68 };
enum : uint8_t { kDhcpDiscover = 1, kDhcpOffer = 2, kDhcpRequest = 3, kDhcpAck = 5, kDhcpNak = 6 };
enum : size_t { kEth = 14, kIp = 20, kUdp = 8, kBootp = 236, kDhcpMin = 300 };

// Ethernet + IPv4 header (no options) in front of `payload` bytes; returns the offset of the payload.
static inline size_t buildIp(uint8_t *f, const uint8_t dst[6], const uint8_t src[6], uint32_t sip, uint32_t dip, uint8_t proto,
                             size_t payload, uint16_t id)
{
  copy(f, dst, 6);
  copy(f + 6, src, 6);
  putBe16(f + 12, kEthIpv4);
  uint8_t *ip = f + kEth;
  zero(ip, kIp);
  ip[0] = 0x45;
  putBe16(ip + 2, uint16_t(kIp + payload));
  putBe16(ip + 4, id);
  ip[8] = 64;
  ip[9] = proto;
  putBe32(ip + 12, sip);
  putBe32(ip + 16, dip);
  putBe16(ip + 10, inetChecksum(ip, kIp));
  return kEth + kIp;
}

// A DHCP client message (DISCOVER, or REQUEST for `want` from `server`), broadcast, UDP checksum 0 (allowed in IPv4).
static inline size_t buildDhcp(uint8_t *f, size_t cap, const uint8_t mac[6], uint32_t xid, uint8_t type, uint32_t want, uint32_t server)
{
  const size_t bootpLen = kDhcpMin;
  const size_t total = kEth + kIp + kUdp + bootpLen;
  if (cap < total) return 0;
  static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  size_t at = buildIp(f, bcast, mac, 0, 0xffffffffu, kIpUdp, kUdp + bootpLen, uint16_t(xid));
  uint8_t *udp = f + at;
  putBe16(udp, kDhcpClientPort);
  putBe16(udp + 2, kDhcpServerPort);
  putBe16(udp + 4, uint16_t(kUdp + bootpLen));
  putBe16(udp + 6, 0);
  uint8_t *b = udp + kUdp;
  zero(b, bootpLen);
  b[0] = 1;   // BOOTREQUEST
  b[1] = 1;   // Ethernet
  b[2] = 6;
  putBe32(b + 4, xid);
  putBe16(b + 10, 0x8000);   // broadcast the reply: we have no address yet
  copy(b + 28, mac, 6);
  size_t o = kBootp;
  putBe32(b + o, 0x63825363); o += 4;   // magic cookie
  b[o++] = 53; b[o++] = 1; b[o++] = type;
  b[o++] = 61; b[o++] = 7; b[o++] = 1; copy(b + o, mac, 6); o += 6;   // client id: hardware type + MAC
  if (type == kDhcpRequest) {
    b[o++] = 50; b[o++] = 4; putBe32(b + o, want); o += 4;
    b[o++] = 54; b[o++] = 4; putBe32(b + o, server); o += 4;
  }
  b[o++] = 55; b[o++] = 6; b[o++] = 1; b[o++] = 3; b[o++] = 6; b[o++] = 26; b[o++] = 51; b[o++] = 54;   // params
  b[o++] = 57; b[o++] = 2; putBe16(b + o, 1500); o += 2;                                               // max message
  b[o++] = 12; b[o++] = 6; copy(b + o, reinterpret_cast<const uint8_t *>("fajita"), 6); o += 6;          // host name
  b[o++] = 255;
  return total;
}

struct DhcpReply {
  uint8_t type, serverMac[6];
  uint32_t yiaddr, server, mask, router, dns, lease, mtu;
};

// Parse a DHCP server reply to `xid` (Ethernet/IPv4/UDP 67->68). False for anything else.
static inline bool parseDhcp(const uint8_t *f, size_t len, uint32_t xid, DhcpReply &r)
{
  if (len < kEth + kIp || getBe16(f + 12) != kEthIpv4) return false;
  const uint8_t *ip = f + kEth;
  size_t ihl = size_t(ip[0] & 0xf) * 4;
  if ((ip[0] >> 4) != 4 || ihl < kIp || ip[9] != kIpUdp || (getBe16(ip + 6) & 0x3fff)) return false;   // no fragments
  size_t ipLen = getBe16(ip + 2);
  if (ipLen < ihl + kUdp || kEth + ipLen > len) return false;
  const uint8_t *udp = ip + ihl;
  if (getBe16(udp) != kDhcpServerPort || getBe16(udp + 2) != kDhcpClientPort) return false;
  size_t udpLen = getBe16(udp + 4);
  if (udpLen < kUdp + kBootp + 4 || udpLen > ipLen - ihl) return false;
  const uint8_t *b = udp + kUdp;
  size_t bl = udpLen - kUdp;
  if (b[0] != 2 || getBe32(b + 4) != xid || getBe32(b + kBootp) != 0x63825363) return false;
  r = DhcpReply{};
  copy(r.serverMac, f + 6, 6);
  r.yiaddr = getBe32(b + 16);
  for (size_t o = kBootp + 4; o < bl;) {
    uint8_t code = b[o];
    if (code == 255) break;
    if (code == 0) { o++; continue; }
    if (o + 2 > bl || o + 2 + b[o + 1] > bl) return false;
    uint8_t n = b[o + 1];
    const uint8_t *v = b + o + 2;
    if (code == 53 && n >= 1) r.type = v[0];
    else if (code == 1 && n >= 4) r.mask = getBe32(v);
    else if (code == 3 && n >= 4) r.router = getBe32(v);
    else if (code == 6 && n >= 4) r.dns = getBe32(v);
    else if (code == 51 && n >= 4) r.lease = getBe32(v);
    else if (code == 54 && n >= 4) r.server = getBe32(v);
    else if (code == 26 && n >= 2) r.mtu = getBe16(v);
    o += 2 + n;
  }
  return r.type != 0;
}

static inline size_t buildArpRequest(uint8_t *f, size_t cap, const uint8_t mac[6], uint32_t sip, uint32_t tip)
{
  const size_t total = 60;   // 42 bytes padded to the Ethernet minimum
  if (cap < total) return 0;
  zero(f, total);
  for (int i = 0; i < 6; i++) f[i] = 0xff;
  copy(f + 6, mac, 6);
  putBe16(f + 12, kEthArp);
  uint8_t *a = f + kEth;
  putBe16(a, 1);
  putBe16(a + 2, kEthIpv4);
  a[4] = 6;
  a[5] = 4;
  putBe16(a + 6, 1);   // request
  copy(a + 8, mac, 6);
  putBe32(a + 14, sip);
  putBe32(a + 24, tip);
  return total;
}

// An ARP reply that says `tip` is at `out`.
static inline bool parseArpReply(const uint8_t *f, size_t len, uint32_t tip, uint8_t out[6])
{
  if (len < kEth + 28 || getBe16(f + 12) != kEthArp) return false;
  const uint8_t *a = f + kEth;
  if (getBe16(a) != 1 || getBe16(a + 2) != kEthIpv4 || a[4] != 6 || a[5] != 4 || getBe16(a + 6) != 2 || getBe32(a + 14) != tip)
    return false;
  copy(out, a + 8, 6);
  return true;
}

static const char kPingText[] = "fajita usbnet ping 0123456789abcdef";   // 35 bytes of echo payload

static inline size_t buildPing(uint8_t *f, size_t cap, const uint8_t dst[6], const uint8_t mac[6], uint32_t sip, uint32_t dip,
                               uint16_t id, uint16_t seq)
{
  const size_t data = sizeof kPingText - 1, icmpLen = 8 + data, total = kEth + kIp + icmpLen;
  if (cap < total) return 0;
  size_t at = buildIp(f, dst, mac, sip, dip, kIpIcmp, icmpLen, seq);
  uint8_t *ic = f + at;
  ic[0] = 8;   // echo request
  ic[1] = 0;
  putBe16(ic + 2, 0);
  putBe16(ic + 4, id);
  putBe16(ic + 6, seq);
  copy(ic + 8, reinterpret_cast<const uint8_t *>(kPingText), data);
  putBe16(ic + 2, inetChecksum(ic, icmpLen));
  return total < 60 ? 60 : total;   // pad to the Ethernet minimum (the buffer is larger)
}

// An ICMP echo reply from `from` with our id and seq whose checksum and payload check out.
static inline bool parsePingReply(const uint8_t *f, size_t len, uint32_t from, uint16_t id, uint16_t seq)
{
  if (len < kEth + kIp + 8 || getBe16(f + 12) != kEthIpv4) return false;
  const uint8_t *ip = f + kEth;
  size_t ihl = size_t(ip[0] & 0xf) * 4;
  if ((ip[0] >> 4) != 4 || ihl < kIp || ip[9] != kIpIcmp || getBe32(ip + 12) != from) return false;
  size_t ipLen = getBe16(ip + 2);
  if (ipLen < ihl + 8 || kEth + ipLen > len) return false;
  const uint8_t *ic = ip + ihl;
  size_t icLen = ipLen - ihl;
  if (ic[0] != 0 || ic[1] != 0 || inetChecksum(ic, icLen) != 0) return false;
  if (getBe16(ic + 4) != id || getBe16(ic + 6) != seq) return false;
  return icLen == 8 + sizeof kPingText - 1 && same(ic + 8, reinterpret_cast<const uint8_t *>(kPingText), sizeof kPingText - 1);
}

typedef int (*SendFn)(void *ctx, const uint8_t *frame, size_t len);   // 0 when queued
typedef void (*LogFn)(void *ctx, const char *text);
typedef void (*RxFn)(void *ctx, const void *frame, size_t len);

static inline int fmtIp(char *out, size_t cap, uint32_t a)
{
  return snprintf(out, cap, "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

static inline bool parseIp(const char *s, size_t n, uint32_t *out)
{
  uint32_t v = 0, part = 0;
  int parts = 0, digits = 0;
  for (size_t i = 0; i <= n; i++) {
    char c = i < n ? s[i] : '.';
    if (c >= '0' && c <= '9') { part = part * 10 + uint32_t(c - '0'); if (++digits > 3 || part > 255) return false; continue; }
    if (c != '.' || !digits || parts == 4) return false;
    v = (v << 8) | part;
    parts++;
    part = 0;
    digits = 0;
  }
  if (parts != 4) return false;
  *out = v;
  return true;
}

// The DHCP + ARP + ping probe. Retries each request every kRetryMs, kTries times, then fails at that step.
struct Probe {
  enum State : uint8_t { kIdle, kDiscover, kRequest, kArp, kPing, kDone, kFailed };
  enum : uint32_t { kRetryMs = 1000, kTries = 4 };
  State state;
  uint8_t mac[6], gwMac[6];
  uint32_t xid, pingTarget, pingTargetArg;
  DhcpReply offer, ack;
  uint64_t startMs, stepMs, pingSentMs;
  uint32_t tries, pingRttMs, runs;
  uint16_t pingId, pingSeq;
  char rows[640];
  size_t rowsLen;

  bool active() const { return state != kIdle && state != kDone && state != kFailed; }
};

struct Net {
  SendFn send;
  LogFn log;
  void *ctx;
  uint64_t (*nowMs)();
  bool echo;
  Probe probe;
  RxFn rxFn;
  void *rxCtx;
  uint32_t rx, rxBytes, rxDropped, echoed, probeRx, delivered;
  uint32_t txProbe, txProbeFail;

  void row(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

  void sendProbe(const uint8_t *f, size_t n)
  {
    txProbe++;
    if (!n || send(ctx, f, n) != 0) txProbeFail++;
  }

  void sendStep()
  {
    static uint8_t f[kMaxFrame];
    Probe &p = probe;
    size_t n = 0;
    if (p.state == Probe::kDiscover) n = buildDhcp(f, sizeof f, p.mac, p.xid, kDhcpDiscover, 0, 0);
    else if (p.state == Probe::kRequest) n = buildDhcp(f, sizeof f, p.mac, p.xid, kDhcpRequest, p.offer.yiaddr, p.offer.server);
    else if (p.state == Probe::kArp) n = buildArpRequest(f, sizeof f, p.mac, p.ack.yiaddr, p.pingTarget);
    else if (p.state == Probe::kPing) {
      p.pingSeq++;
      n = buildPing(f, sizeof f, p.gwMac, p.mac, p.ack.yiaddr, p.pingTarget, p.pingId, p.pingSeq);
      p.pingSentMs = nowMs();
    }
    if (n) sendProbe(f, n);
    p.stepMs = nowMs();
    p.tries++;
  }
  void enter(Probe::State s)
  {
    probe.state = s;
    probe.tries = 0;
    sendStep();
  }

  // Start the probe; `target` 0 pings the router the server names.
  void startProbe(uint32_t target)
  {
    Probe &p = probe;
    uint32_t runs = p.runs + 1;
    uint8_t mac[6];
    copy(mac, p.mac, 6);
    p = Probe{};
    copy(p.mac, mac, 6);
    p.runs = runs;
    p.startMs = nowMs();
    p.xid = 0x46a70000u ^ uint32_t(p.startMs * 2654435761u) ^ runs;
    p.pingId = uint16_t(0xfa00 | (runs & 0xff));
    p.pingTargetArg = target;
    row("usbnet: probe %u: DHCP DISCOVER xid %08x from %02x:%02x:%02x:%02x:%02x:%02x", runs, p.xid, mac[0], mac[1], mac[2], mac[3],
        mac[4], mac[5]);
    enter(Probe::kDiscover);
  }

  void fail(const char *why)
  {
    probe.state = Probe::kFailed;
    row("usbnet: probe FAILED: %s (after %llu ms)", why, (unsigned long long)(nowMs() - probe.startMs));
  }

  // Timeouts and retries; call every few milliseconds.
  void tick()
  {
    Probe &p = probe;
    if (!p.active() || nowMs() - p.stepMs < Probe::kRetryMs) return;
    if (p.tries >= Probe::kTries) {
      static const char *const step[] = {"", "no DHCP OFFER", "no DHCP ACK", "no ARP reply", "no ICMP echo reply"};
      fail(step[p.state]);
      return;
    }
    sendStep();
  }

  bool probeFrame(const uint8_t *f, size_t len)
  {
    Probe &p = probe;
    char a[16], b[16], c[16], d[16];
    if (p.state == Probe::kDiscover || p.state == Probe::kRequest) {
      DhcpReply r;
      if (!parseDhcp(f, len, p.xid, r)) return false;
      if (p.state == Probe::kDiscover && r.type == kDhcpOffer) {
        p.offer = r;
        fmtIp(a, sizeof a, r.yiaddr); fmtIp(b, sizeof b, r.server); fmtIp(c, sizeof c, r.router); fmtIp(d, sizeof d, r.dns);
        row("usbnet: DHCP OFFER %s from %s (%02x:%02x:%02x:%02x:%02x:%02x) router %s dns %s lease %u mtu %u, %llu ms", a, b,
            r.serverMac[0], r.serverMac[1], r.serverMac[2], r.serverMac[3], r.serverMac[4], r.serverMac[5], c, d, r.lease, r.mtu,
            (unsigned long long)(nowMs() - p.startMs));
        enter(Probe::kRequest);
      } else if (p.state == Probe::kRequest && r.type == kDhcpAck) {
        p.ack = r;
        if (!r.yiaddr) { fail("ACK without an address"); return true; }
        fmtIp(a, sizeof a, r.yiaddr); fmtIp(b, sizeof b, r.mask);
        row("usbnet: DHCP ACK %s mask %s lease %u, %llu ms", a, b, r.lease, (unsigned long long)(nowMs() - p.startMs));
        p.pingTarget = p.pingTargetArg ? p.pingTargetArg : r.router;
        if (!p.pingTarget) { p.state = Probe::kDone; row("usbnet: probe done (no router to ping)"); return true; }
        enter(Probe::kArp);
      } else if (r.type == kDhcpNak) {
        fail("DHCP NAK");
      }
      return true;
    }
    if (p.state == Probe::kArp) {
      if (!parseArpReply(f, len, p.pingTarget, p.gwMac)) return false;
      fmtIp(a, sizeof a, p.pingTarget);
      row("usbnet: ARP %s is at %02x:%02x:%02x:%02x:%02x:%02x", a, p.gwMac[0], p.gwMac[1], p.gwMac[2], p.gwMac[3], p.gwMac[4],
          p.gwMac[5]);
      enter(Probe::kPing);
      return true;
    }
    if (p.state == Probe::kPing) {
      if (!parsePingReply(f, len, p.pingTarget, p.pingId, p.pingSeq)) return false;
      p.pingRttMs = uint32_t(nowMs() - p.pingSentMs);
      p.state = Probe::kDone;
      fmtIp(a, sizeof a, p.pingTarget);
      row("usbnet: ICMP echo reply from %s seq %u rtt %u ms", a, p.pingSeq, p.pingRttMs);
      row("usbnet: probe PASS: DHCP, ARP and ping over the NET channel in %llu ms", (unsigned long long)(nowMs() - p.startMs));
      return true;
    }
    return false;
  }

  // A frame the host sent (poll thread). Valid only for the call.
  void onFrame(const uint8_t *f, size_t len)
  {
    rx++;
    rxBytes += uint32_t(len);
    if (echo) {
      if (send(ctx, f, len) == 0) echoed++;
      return;
    }
    if (probe.active() && probeFrame(f, len)) { probeRx++; return; }
    RxFn fn = __atomic_load_n(&rxFn, __ATOMIC_ACQUIRE);
    if (fn) { delivered++; fn(rxCtx, f, len); return; }
    rxDropped++;
  }

  size_t kcmd(const char *args, size_t len, char *out, size_t cap, const char *transportState);
};

// One row: to the log, and kept (most recent run) for `usbnet show`.
inline void Net::row(const char *fmt, ...)
{
  char line[200];
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line, fmt, ap);
  __builtin_va_end(ap);
  if (n < 0) return;
  size_t l = size_t(n) < sizeof line ? size_t(n) : sizeof line - 1;
  if (log) log(ctx, line);
  Probe &p = probe;
  if (p.rowsLen + l + 2 <= sizeof p.rows) {
    copy(reinterpret_cast<uint8_t *>(p.rows + p.rowsLen), reinterpret_cast<const uint8_t *>(line), l);
    p.rowsLen += l;
    p.rows[p.rowsLen++] = '\n';
    p.rows[p.rowsLen] = 0;
  }
}

static inline bool word(const char *&s, size_t &n, const char *w)
{
  size_t l = 0;
  while (w[l]) l++;
  if (n < l || !same(reinterpret_cast<const uint8_t *>(s), reinterpret_cast<const uint8_t *>(w), l) || (n > l && s[l] != ' ')) return false;
  s += l; n -= l;
  while (n && *s == ' ') { s++; n--; }
  return true;
}

inline size_t Net::kcmd(const char *s, size_t n, char *out, size_t cap, const char *transportState)
{
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\0')) n--;
  while (n && *s == ' ') { s++; n--; }
  int w = 0;
  static const char *const names[] = {"idle", "discover", "request", "arp", "ping", "done", "failed"};
  if (word(s, n, "echo")) {
    if (word(s, n, "on")) echo = true;
    else if (word(s, n, "off")) echo = false;
    w = snprintf(out, cap, "usbnet echo %s (echoed %u)\n", echo ? "on" : "off", echoed);
  } else if (word(s, n, "dhcp")) {
    uint32_t target = 0;
    if (n && !parseIp(s, n, &target)) return size_t(snprintf(out, cap, "usage: usbnet dhcp [A.B.C.D]\n"));
    if (echo) return size_t(snprintf(out, cap, "usbnet: echo is on; usbnet echo off first\n"));
    startProbe(target);
    w = snprintf(out, cap, "usbnet probe %u started (xid %08x); usbnet show for the result\n", probe.runs, probe.xid);
  } else if (word(s, n, "show")) {
    w = snprintf(out, cap, "probe %u state %s\n%s", probe.runs, names[probe.state], probe.rows);
  } else if (n == 0 || word(s, n, "state")) {
    w = snprintf(out, cap,
                 "%secho=%u rx=%u rx_bytes=%u rx_dropped=%u echoed=%u delivered=%u receiver=%s probe=%s probe_rx=%u probe_tx=%u "
                 "probe_tx_fail=%u\n",
                 transportState ? transportState : "", echo, rx, rxBytes, rxDropped, echoed, delivered,
                 __atomic_load_n(&rxFn, __ATOMIC_ACQUIRE) ? "yes" : "no", names[probe.state], probeRx, txProbe, txProbeFail);
  } else {
    w = snprintf(out, cap, "usage: usbnet [state] | echo on|off | dhcp [A.B.C.D] | show\n");
  }
  return w < 0 ? 0 : (size_t(w) < cap ? size_t(w) : cap - 1);
}

}  // namespace usbnet

#endif
