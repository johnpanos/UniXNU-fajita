// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of UsbNet.h, the NET channel core: frame builders against hand-checked fields and checksums, the parsers
// on good and malformed replies, echo and receiver dispatch, and the probe's state machine against a scripted DHCP
// server (OFFER, ACK, ARP reply, ICMP echo reply), its retries and its failure on silence.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../UsbNet.h"

using namespace usbnet;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static uint64_t gNow = 1000;
static uint64_t now() { return gNow; }
static std::vector<std::vector<uint8_t>> gSent;
static int gSendRc = 0;
static int sendFn(void *, const uint8_t *f, size_t n) { if (gSendRc) return gSendRc; gSent.emplace_back(f, f + n); return 0; }
static std::vector<std::string> gRows;
static void logFn(void *, const char *t) { gRows.emplace_back(t); }
static std::vector<std::vector<uint8_t>> gDelivered;
static void rxFn(void *ctx, const void *f, size_t n)
{
  CHECK(ctx == &gDelivered);
  gDelivered.emplace_back(static_cast<const uint8_t *>(f), static_cast<const uint8_t *>(f) + n);
}

static const uint8_t kMac[6] = {0x02, 0xfa, 0x6a, 0x17, 0x4e, 0x01};
static const uint8_t kSrvMac[6] = {0x9a, 0x55, 0x9a, 0x55, 0x9a, 0x55};
static const uint32_t kYi = 0xc0a80142, kSrv = 0xc0a80101, kMask = 0xffffff00, kDns = 0xc0a80101;

static bool hasRow(const char *needle)
{
  for (auto &r : gRows) if (strstr(r.c_str(), needle)) return true;
  return false;
}

// A server reply: Ethernet/IPv4/UDP 67 -> 68, BOOTREPLY with the given options.
static std::vector<uint8_t> dhcpReply(uint32_t xid, uint8_t type, uint32_t yi, bool router = true)
{
  uint8_t opts[64];
  size_t o = 0;
  opts[o++] = 53; opts[o++] = 1; opts[o++] = type;
  opts[o++] = 54; opts[o++] = 4; putBe32(opts + o, kSrv); o += 4;
  opts[o++] = 1; opts[o++] = 4; putBe32(opts + o, kMask); o += 4;
  if (router) { opts[o++] = 3; opts[o++] = 4; putBe32(opts + o, kSrv); o += 4; }
  opts[o++] = 6; opts[o++] = 4; putBe32(opts + o, kDns); o += 4;
  opts[o++] = 51; opts[o++] = 4; putBe32(opts + o, 3600); o += 4;
  opts[o++] = 0;   // a pad byte inside the options
  opts[o++] = 26; opts[o++] = 2; putBe16(opts + o, 1500); o += 2;
  opts[o++] = 255;
  size_t bl = kBootp + 4 + o;
  std::vector<uint8_t> f(kEth + kIp + kUdp + bl);
  size_t at = buildIp(f.data(), kMac, kSrvMac, kSrv, yi, kIpUdp, kUdp + bl, 7);
  uint8_t *u = f.data() + at;
  putBe16(u, 67); putBe16(u + 2, 68); putBe16(u + 4, uint16_t(kUdp + bl));
  uint8_t *b = u + kUdp;
  b[0] = 2; b[1] = 1; b[2] = 6;
  putBe32(b + 4, xid);
  putBe32(b + 16, yi);
  memcpy(b + 28, kMac, 6);
  putBe32(b + kBootp, 0x63825363);
  memcpy(b + kBootp + 4, opts, o);
  return f;
}

static std::vector<uint8_t> arpReply(uint32_t ip, const uint8_t mac[6])
{
  std::vector<uint8_t> f(60);
  memcpy(f.data(), kMac, 6); memcpy(f.data() + 6, mac, 6); putBe16(f.data() + 12, kEthArp);
  uint8_t *a = f.data() + kEth;
  putBe16(a, 1); putBe16(a + 2, kEthIpv4); a[4] = 6; a[5] = 4; putBe16(a + 6, 2);
  memcpy(a + 8, mac, 6); putBe32(a + 14, ip); memcpy(a + 18, kMac, 6); putBe32(a + 24, kYi);
  return f;
}

// Turn our echo request into the reply a host would send.
static std::vector<uint8_t> pingReply(const std::vector<uint8_t> &req)
{
  std::vector<uint8_t> f = req;
  memcpy(f.data(), req.data() + 6, 6); memcpy(f.data() + 6, req.data(), 6);
  uint8_t *ip = f.data() + kEth;
  uint8_t t[4]; memcpy(t, ip + 12, 4); memcpy(ip + 12, ip + 16, 4); memcpy(ip + 16, t, 4);
  putBe16(ip + 10, 0); putBe16(ip + 10, inetChecksum(ip, kIp));
  uint8_t *ic = ip + kIp;
  size_t icLen = getBe16(ip + 2) - kIp;
  ic[0] = 0; putBe16(ic + 2, 0); putBe16(ic + 2, inetChecksum(ic, icLen));
  return f;
}

static void reset(Net &n)
{
  n = Net{};
  n.send = sendFn; n.log = logFn; n.nowMs = now;
  memcpy(n.probe.mac, kMac, 6);
  gSent.clear(); gRows.clear(); gDelivered.clear(); gSendRc = 0;
}

static void testBuilders()
{
  uint8_t f[kMaxFrame];
  size_t n = buildDhcp(f, sizeof f, kMac, 0x11223344, kDhcpDiscover, 0, 0);
  CHECK(n == kEth + kIp + kUdp + kDhcpMin);
  CHECK(!memcmp(f, "\xff\xff\xff\xff\xff\xff", 6) && !memcmp(f + 6, kMac, 6) && getBe16(f + 12) == kEthIpv4);
  const uint8_t *ip = f + kEth;
  CHECK(ip[0] == 0x45 && getBe16(ip + 2) == n - kEth && ip[9] == kIpUdp && inetChecksum(ip, kIp) == 0);
  CHECK(getBe32(ip + 12) == 0 && getBe32(ip + 16) == 0xffffffffu);
  const uint8_t *u = ip + kIp, *b = u + kUdp;
  CHECK(getBe16(u) == 68 && getBe16(u + 2) == 67 && getBe16(u + 4) == n - kEth - kIp);
  CHECK(b[0] == 1 && getBe32(b + 4) == 0x11223344 && getBe16(b + 10) == 0x8000 && !memcmp(b + 28, kMac, 6));
  CHECK(getBe32(b + kBootp) == 0x63825363 && b[kBootp + 4] == 53 && b[kBootp + 6] == kDhcpDiscover);
  CHECK(buildDhcp(f, 100, kMac, 1, kDhcpDiscover, 0, 0) == 0);
  // REQUEST carries the requested address (50) and the server id (54).
  n = buildDhcp(f, sizeof f, kMac, 5, kDhcpRequest, kYi, kSrv);
  b = f + kEth + kIp + kUdp;
  bool r50 = false, r54 = false;
  for (size_t o = kBootp + 4; b[o] != 255; o += 2 + b[o + 1]) {
    if (b[o] == 50) r50 = getBe32(b + o + 2) == kYi;
    if (b[o] == 54) r54 = getBe32(b + o + 2) == kSrv;
  }
  CHECK(r50 && r54);
  // RFC 1071 example bytes: 00 01 f2 03 f4 f5 f6 f7 sum to 0xddf2, checksum 0x220d.
  const uint8_t ex[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
  CHECK(inetChecksum(ex, sizeof ex) == 0x220d);
  n = buildArpRequest(f, sizeof f, kMac, kYi, kSrv);
  CHECK(n == 60 && getBe16(f + 12) == kEthArp && getBe16(f + kEth + 6) == 1 && getBe32(f + kEth + 24) == kSrv);
  n = buildPing(f, sizeof f, kSrvMac, kMac, kYi, kSrv, 0xfa01, 3);
  CHECK(n == kEth + kIp + 8 + sizeof kPingText - 1 && inetChecksum(f + kEth, kIp) == 0 && inetChecksum(f + kEth + kIp, 8 + sizeof kPingText - 1) == 0);
  uint32_t a;
  CHECK(parseIp("10.0.2.2", 8, &a) && a == 0x0a000202);
  CHECK(!parseIp("10.0.2", 6, &a) && !parseIp("1.2.3.256", 9, &a) && !parseIp("1..2.3", 6, &a) && !parseIp("1.2.3.4.5", 9, &a));
  printf("UsbNet: builders, checksums, parseIp OK\n");
}

static void testParsers()
{
  DhcpReply r;
  auto f = dhcpReply(42, kDhcpOffer, kYi);
  CHECK(parseDhcp(f.data(), f.size(), 42, r));
  CHECK(r.type == kDhcpOffer && r.yiaddr == kYi && r.server == kSrv && r.router == kSrv && r.dns == kDns && r.mask == kMask);
  CHECK(r.lease == 3600 && r.mtu == 1500 && !memcmp(r.serverMac, kSrvMac, 6));
  CHECK(!parseDhcp(f.data(), f.size(), 43, r));        // another client's transaction
  CHECK(!parseDhcp(f.data(), 40, 42, r));              // truncated
  auto g = f; g[kEth + kIp + kUdp + kBootp] ^= 1;       // bad magic cookie
  CHECK(!parseDhcp(g.data(), g.size(), 42, r));
  g = f; g[kEth + kIp + kUdp + kBootp + 5] = 200;       // an option running past the end
  CHECK(!parseDhcp(g.data(), g.size(), 42, r));
  g = f; putBe16(g.data() + kEth + 6, 0x2000);          // a fragment
  CHECK(!parseDhcp(g.data(), g.size(), 42, r));
  uint8_t mac[6];
  auto a = arpReply(kSrv, kSrvMac);
  CHECK(parseArpReply(a.data(), a.size(), kSrv, mac) && !memcmp(mac, kSrvMac, 6));
  CHECK(!parseArpReply(a.data(), a.size(), kSrv + 1, mac));
  uint8_t p[kMaxFrame];
  size_t n = buildPing(p, sizeof p, kSrvMac, kMac, kYi, kSrv, 0xfa01, 3);
  auto rep = pingReply(std::vector<uint8_t>(p, p + n));
  CHECK(parsePingReply(rep.data(), rep.size(), kSrv, 0xfa01, 3));
  CHECK(!parsePingReply(rep.data(), rep.size(), kSrv, 0xfa01, 4));
  rep[kEth + kIp + 10] ^= 0x20;                         // payload corrupted (checksum no longer matches)
  CHECK(!parsePingReply(rep.data(), rep.size(), kSrv, 0xfa01, 3));
  printf("UsbNet: DHCP/ARP/ICMP parsers OK\n");
}

static void testDispatch()
{
  Net n;
  reset(n);
  uint8_t f[1514];
  for (size_t i = 0; i < sizeof f; i++) f[i] = uint8_t(i * 7);
  n.onFrame(f, sizeof f);
  CHECK(n.rx == 1 && n.rxDropped == 1 && gSent.empty());   // no receiver: dropped
  n.rxCtx = &gDelivered;
  n.rxFn = rxFn;
  n.onFrame(f, 60);
  CHECK(gDelivered.size() == 1 && gDelivered[0] == std::vector<uint8_t>(f, f + 60) && n.delivered == 1);
  char out[512];
  size_t l = n.kcmd("echo on", 7, out, sizeof out, "");
  CHECK(n.echo && strstr(out, "echo on"));
  n.onFrame(f, sizeof f);
  CHECK(gSent.size() == 1 && gSent[0] == std::vector<uint8_t>(f, f + sizeof f) && n.echoed == 1 && gDelivered.size() == 1);
  l = n.kcmd("dhcp", 4, out, sizeof out, "");
  CHECK(strstr(out, "echo is on") && n.probe.state == Probe::kIdle);
  n.kcmd("echo off", 8, out, sizeof out, "");
  l = n.kcmd("", 0, out, sizeof out, "link=1 ");
  CHECK(l > 0 && strstr(out, "link=1 echo=0 rx=3 ") && strstr(out, "receiver=yes"));
  n.kcmd("bogus", 5, out, sizeof out, "");
  CHECK(strstr(out, "usage:"));
  n.kcmd("dhcp 1.2.3", 10, out, sizeof out, "");
  CHECK(strstr(out, "usage:"));
  printf("UsbNet: dispatch (drop, receiver, echo) and kcmd OK\n");
}

static void testProbe()
{
  Net n;
  reset(n);
  n.rxCtx = &gDelivered;
  n.rxFn = rxFn;
  char out[1024];
  n.kcmd("dhcp", 4, out, sizeof out, "");
  CHECK(n.probe.state == Probe::kDiscover && gSent.size() == 1);
  uint32_t xid = n.probe.xid;
  DhcpReply r;
  // our own DISCOVER, if it came back, is not a server reply
  CHECK(!parseDhcp(gSent[0].data(), gSent[0].size(), xid, r));
  // an unrelated frame during the probe still reaches the receiver
  uint8_t junk[60] = {};
  n.onFrame(junk, sizeof junk);
  CHECK(gDelivered.size() == 1);
  // no answer for one retry period: DISCOVER again
  gNow += 1000; n.tick();
  CHECK(gSent.size() == 2 && n.probe.tries == 2);
  auto offer = dhcpReply(xid, kDhcpOffer, kYi);
  gNow += 5;
  n.onFrame(offer.data(), offer.size());
  CHECK(n.probe.state == Probe::kRequest && gSent.size() == 3 && hasRow("DHCP OFFER 192.168.1.66 from 192.168.1.1"));
  auto ack = dhcpReply(xid, kDhcpAck, kYi);
  n.onFrame(ack.data(), ack.size());
  CHECK(n.probe.state == Probe::kArp && gSent.size() == 4 && getBe16(gSent[3].data() + 12) == kEthArp);
  auto arp = arpReply(kSrv, kSrvMac);
  n.onFrame(arp.data(), arp.size());
  CHECK(n.probe.state == Probe::kPing && gSent.size() == 5 && !memcmp(gSent[4].data(), kSrvMac, 6));
  gNow += 3;
  auto pong = pingReply(gSent[4]);
  n.onFrame(pong.data(), pong.size());
  CHECK(n.probe.state == Probe::kDone && n.probe.pingRttMs == 3 && hasRow("probe PASS") && n.probeRx == 4);
  n.kcmd("show", 4, out, sizeof out, "");
  CHECK(strstr(out, "state done") && strstr(out, "DHCP ACK 192.168.1.66 mask 255.255.255.0") && strstr(out, "rtt 3 ms"));
  printf("%s", out);

  // a given ping target, and silence at the ARP step: four tries, then FAILED
  reset(n);
  n.kcmd("dhcp 10.0.2.3", 13, out, sizeof out, "");
  xid = n.probe.xid;
  offer = dhcpReply(xid, kDhcpOffer, kYi);
  n.onFrame(offer.data(), offer.size());
  ack = dhcpReply(xid, kDhcpAck, kYi);
  n.onFrame(ack.data(), ack.size());
  CHECK(n.probe.state == Probe::kArp && getBe32(gSent.back().data() + kEth + 24) == 0x0a000203);
  for (int i = 0; i < 4; i++) { gNow += 1000; n.tick(); }
  CHECK(n.probe.state == Probe::kFailed && hasRow("FAILED: no ARP reply"));
  CHECK(gSent.size() == 2 + 4);   // DISCOVER, REQUEST, 4 ARP requests

  // a NAK ends the probe; a send failure is counted and retried
  reset(n);
  gSendRc = -3;
  n.kcmd("dhcp", 4, out, sizeof out, "");
  CHECK(n.txProbeFail == 1);
  gSendRc = 0;
  xid = n.probe.xid;
  offer = dhcpReply(xid, kDhcpOffer, kYi);
  n.onFrame(offer.data(), offer.size());
  auto nak = dhcpReply(xid, kDhcpNak, 0);
  n.onFrame(nak.data(), nak.size());
  CHECK(n.probe.state == Probe::kFailed && hasRow("DHCP NAK"));
  // no router in the ACK and no target: done after the lease
  reset(n);
  n.kcmd("dhcp", 4, out, sizeof out, "");
  xid = n.probe.xid;
  offer = dhcpReply(xid, kDhcpOffer, kYi, false);
  n.onFrame(offer.data(), offer.size());
  ack = dhcpReply(xid, kDhcpAck, kYi, false);
  n.onFrame(ack.data(), ack.size());
  CHECK(n.probe.state == Probe::kDone && hasRow("no router"));
  printf("UsbNet: probe (DORA, ARP, ping, retries, failures) OK\n");
}

int main()
{
  testBuilders();
  testParsers();
  testDispatch();
  testProbe();
  printf("UsbNet: PASS\n");
  return 0;
}
