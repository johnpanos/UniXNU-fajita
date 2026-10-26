// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host simulator of the device end of the fjdbg channel: the same UsbDbgProto.h /
// UsbDbgDevice.h code the kernel will run, over stdin/stdout instead of the DWC3
// bulk pair, with a fake memory. tests/run.sh drives it with scripts/fajita-usbdbg.py
// (`--exec`), so the host tool is tested against the real device-side code.
//
//   UsbDbgSim [--writes]
// Memory: 4 KiB of "kernel" at 0xfffffff007004000 (bytes i & 0xff) and the same
// bytes at physical 0x80000000; MMIO: GSNPSID 0x5533330a at 0x0a60c120.
// On start it prints a console line and one LOG frame, like the kernel's banner.
// NET channel: always on, with the kernel's UsbNet.h core (kcmd usbnet ...: echo, the DHCP/ARP/ping probe); probe rows
// go out as console text.
#include <cstdio>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <unistd.h>
#include "../UsbDbgDevice.h"
#include "../UsbNet.h"

using namespace usbdbg;

static uint8_t gMem[4096];
static const uint64_t kVirt = 0xfffffff007004000ull, kPhys = 0x80000000ull;
static const char kLog[] = "sim: kernel log line 1\nsim: kernel log line 2\n";
static int gResets;

static void emit(uint8_t ch, uint16_t seq, const uint8_t *d, size_t n)
{
  static uint8_t wire[kMaxEncoded];
  size_t len = encodeFrame(ch, seq, d, n, wire, sizeof wire);
  if (len && write(1, wire, len) != ssize_t(len)) _exit(2);
}

static int32_t span(uint64_t base, uint64_t a, uint32_t n, uint8_t **p)
{
  if (a < base || a - base + n > sizeof gMem) return kErrFault;
  *p = gMem + (a - base);
  return kOk;
}
static int32_t rv(void *, uint64_t a, void *o, uint32_t n) { uint8_t *p; int32_t s = span(kVirt, a, n, &p); if (!s) memcpy(o, p, n); return s; }
static int32_t wv(void *, uint64_t a, const void *i, uint32_t n) { uint8_t *p; int32_t s = span(kVirt, a, n, &p); if (!s) memcpy(p, i, n); return s; }
static int32_t rp(void *, uint64_t a, void *o, uint32_t n) { uint8_t *p; int32_t s = span(kPhys, a, n, &p); if (!s) memcpy(o, p, n); return s; }
static int32_t r32(void *, uint64_t pa, uint32_t *o, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) o[i] = (pa + 4 * i == 0x0a60c120) ? 0x5533330au : 0;
  return kOk;
}
static size_t state(void *, char *out, size_t cap)
{
  return size_t(snprintf(out, cap, "link=sim\nresets=%d\n", gResets));
}
static usbnet::Net gNet;
static uint64_t nowMs()
{
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}
static int netSend(void *, const uint8_t *f, size_t n) { emit(kChNet, 0, f, n); return 0; }
static void netLog(void *, const char *text)
{
  if (write(1, text, strlen(text)) < 0 || write(1, "\n", 1) < 0) _exit(2);
}

static size_t kcmd(void *, const char *cmd, size_t len, char *out, size_t cap)
{
  if (len >= 6 && memcmp(cmd, "usbnet", 6) == 0 && (len == 6 || cmd[6] == ' '))
    return gNet.kcmd(cmd + 6, len - 6, out, cap, "link=1 sim ");
  return size_t(snprintf(out, cap, "kcmd '%.*s': ok\n", int(len), cmd));
}
static int32_t logFrom(void *, uint64_t off, uint64_t *used)
{
  if (off > sizeof kLog - 1) off = 0;
  *used = off;
  uint8_t data[kMaxData];
  size_t n = buildLogData(off, reinterpret_cast<const uint8_t *>(kLog) + off, sizeof kLog - 1 - off, data);
  emit(kChLog, 0, data, n);
  return kOk;
}
static int32_t reset(void *, uint32_t) { gResets++; return kOk; }

int main(int argc, char **argv)
{
  for (size_t i = 0; i < sizeof gMem; i++) gMem[i] = uint8_t(i);
  Target t{};
  t.ident = "UsbDbgSim";
  t.allowWrites = argc > 1 && strcmp(argv[1], "--writes") == 0;
  t.features = kFeatLog | kFeatMemory | kFeatMmio | kFeatKcmd;
  t.readVirt = rv;
  t.writeVirt = wv;
  t.readPhys = rp;
  t.read32 = r32;
  t.state = state;
  t.kcmd = kcmd;
  t.logFrom = logFrom;
  t.reset = reset;

  const char banner[] = "UsbDbgSim console up\n";
  if (write(1, banner, sizeof banner - 1) < 0) return 2;
  uint64_t used;
  logFrom(nullptr, 0, &used);

  gNet.send = netSend;
  gNet.log = netLog;
  gNet.nowMs = nowMs;
  static const uint8_t mac[6] = {0x02, 0xfa, 0x6a, 0x17, 0x4e, 0x01};
  memcpy(gNet.probe.mac, mac, 6);

  Decoder d;
  alignas(4) static uint8_t rsp[kMaxData];
  uint8_t buf[512];
  for (;;) {
    pollfd pfd = {0, POLLIN, 0};
    int pr = poll(&pfd, 1, 20);
    gNet.tick();
    if (pr == 0) continue;
    ssize_t n = read(0, buf, sizeof buf);
    if (n <= 0) return 0;
    for (ssize_t i = 0; i < n; i++) {
      if (d.feed(buf[i]) != Decoder::kFrame) continue;
      if (d.channel() == kChCmd && d.length() >= 2 && getLe16(d.data()) == kOpShot) {
        // A synthetic 300x7 BGR frame (pixel R,G,B = (7x+y, x+3y, x^y) & 255), in whole-pixel FILE frames like the
        // gadget's; the first frame goes out before the reply, as it can on the device.
        const uint32_t w = 300, h = 7, total = w * h * 3, per = (kMaxData - 8) / 3 * 3;
        static uint8_t img[w * h * 3];
        for (uint32_t y = 0; y < h; y++)
          for (uint32_t x = 0; x < w; x++) {
            uint8_t *px = img + (y * w + x) * 3;
            px[0] = uint8_t(x ^ y); px[1] = uint8_t(x + y * 3); px[2] = uint8_t(x * 7 + y);
          }
        auto frame = [&](uint32_t off) {
          static uint8_t data[kMaxData];
          uint32_t n = total - off < per ? total - off : per;
          putLe32(data, 1); putLe32(data + 4, off);
          memcpy(data + 8, img + off, n);
          emit(kChFile, 0, data, 8 + n);
          return n;
        };
        uint32_t off = frame(0);
        alignas(4) uint8_t r[40];
        putLe16(r, uint16_t(kOpShot | kOpReplyBit)); putLe16(r + 2, 0); putLe32(r + 4, 0);
        putLe32(r + 8, 1); putLe32(r + 12, w); putLe32(r + 16, h); putLe32(r + 20, 3);
        putLe64(r + 24, total); putLe32(r + 32, 2); putLe32(r + 36, w * 8);
        emit(kChRsp, d.seq(), r, sizeof r);
        while (off < total) off += frame(off);
      } else if (d.channel() == kChCmd) {
        size_t len = handleCommand(t, d.data(), d.length(), rsp);
        emit(kChRsp, d.seq(), rsp, len);
      } else if (d.channel() == kChNet) {
        gNet.onFrame(d.data(), d.length());
      } else if (d.channel() == kChShell) {
        // Emulate fjdbg-agent: one command per line; an unknown program prints an error line without an exit
        // marker, anything else "runs" and ends with `fjdbg: exit N` (N = the argument count, to test parsing).
        static char line[512];
        static size_t ll = 0;
        for (size_t k = 0; k < d.length(); k++) {
          char c = char(d.data()[k]);
          if (c != '\n') { if (ll < sizeof line - 1) line[ll++] = c; continue; }
          line[ll] = 0;
          char out[700];
          int n;
          if (strncmp(line, "@read ", 6) == 0) {   // the agent's builtin: file bytes, then "fjdbg: eof"
            n = snprintf(out, sizeof out, "127.0.0.1 localhost\n\nfjdbg: eof\n");
          } else if (strncmp(line, "fjdbg-run", 9) == 0) {
            n = snprintf(out, sizeof out, "fjdbg: cannot run %s: No such file or directory\n", line);
          } else {
            int args = 1;
            for (char *q = line; *q; q++) args += *q == ' ';
            n = snprintf(out, sizeof out, "sim ran: %s\nfjdbg: exit %d\n", line, args);
          }
          emit(kChShell, 0, reinterpret_cast<uint8_t *>(out), size_t(n));
          ll = 0;
        }
      }
    }
  }
}
