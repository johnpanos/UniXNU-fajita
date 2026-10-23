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
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include "../UsbDbgDevice.h"

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
static size_t kcmd(void *, const char *cmd, size_t len, char *out, size_t cap)
{
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

  Decoder d;
  alignas(4) static uint8_t rsp[kMaxData];
  uint8_t buf[512];
  for (;;) {
    ssize_t n = read(0, buf, sizeof buf);
    if (n <= 0) return 0;
    for (ssize_t i = 0; i < n; i++) {
      if (d.feed(buf[i]) == Decoder::kFrame && d.channel() == kChCmd) {
        size_t len = handleCommand(t, d.data(), d.length(), rsp);
        emit(kChRsp, d.seq(), rsp, len);
      }
    }
  }
}
