// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the kernel's fjdbg Target hooks (UsbDbgKernelTarget.h) over stubs of the XNU calls they make: a fake
// register file behind ml_io_map, a fake RAM behind ml_phys_read/write and pmap_valid_address, ml_nofault_copy.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;

// ---- stubs ------------------------------------------------------------------------------------------------------
static std::map<uint64_t, std::vector<uint32_t>> gPages;   // fake MMIO, 4 KiB pages of words
static unsigned gMaps;
static std::vector<uint8_t> gRam(0x4000);                   // fake RAM at 0x80000000..+16 KiB
extern "C" {
volatile uint32_t native_dwc3_gadget_on = 3;
vm_offset_t ml_io_map(vm_offset_t pa, vm_size_t size)
{
  assert(size == 0x1000 && !(pa & 0xfff));
  assert(pa < 0x80000000ull);   // the guard must never map RAM
  gMaps++;
  auto &p = gPages[pa];
  if (p.empty()) p.assign(1024, 0);
  return reinterpret_cast<vm_offset_t>(p.data());
}
static int gResets;
int boot_reset_bootloader(char *, unsigned) { gResets++; return 99; }
int pmap_valid_address(uint64_t a) { return a >= 0x80000000ull && a < 0x80000000ull + gRam.size(); }
unsigned int ml_phys_read_word(vm_offset_t a) { uint32_t v; memcpy(&v, &gRam[a - 0x80000000ull], 4); return v; }
unsigned int ml_phys_read_byte(vm_offset_t a) { return gRam[a - 0x80000000ull]; }
void ml_phys_write_word(vm_offset_t a, unsigned int v) { memcpy(&gRam[a - 0x80000000ull], &v, 4); }
void ml_phys_write_byte(vm_offset_t a, unsigned int v) { gRam[a - 0x80000000ull] = uint8_t(v); }
vm_size_t ml_nofault_copy(vm_offset_t src, vm_offset_t dst, vm_size_t n)
{
  if (!src || !dst) return 0;   // "unmapped"
  memcpy(reinterpret_cast<void *>(dst), reinterpret_cast<const void *>(src), n);
  return n;
}
}
static uint32_t &reg(uint64_t pa)
{
  auto &p = gPages[pa & ~uint64_t(0xfff)];
  if (p.empty()) p.assign(1024, 0);
  return p[(pa & 0xfff) / 4];
}

#include "../UsbDbgKernelTarget.h"

static size_t echoCmd(const char *args, size_t len, char *out, size_t cap)
{
  return size_t(snprintf(out, cap, "echo:%.*s", int(len), args));
}

int main()
{
  using namespace usbdbg;
  Target t{};
  usbdbgk::fill(t, false);
  assert(t.read32 && t.readPhys && t.readVirt && t.kcmd && !t.allowWrites);
  assert(t.features & kFeatMmio);
  alignas(4) uint8_t rsp[kMaxData];
  uint8_t req[64];

  // An unguarded block (TLMM 0x03400000): read through the map, one map per page.
  reg(0x03400010) = 0x12345678;
  uint32_t v[4] = {};
  assert(t.read32(nullptr, 0x03400010, v, 1) == kOk && v[0] == 0x12345678);
  unsigned maps = gMaps;
  assert(t.read32(nullptr, 0x03400014, v, 1) == kOk && gMaps == maps);   // same page: no new map

  // The GPU core (gx) is refused while gpu_gx_gdsc is collapsed, allowed once PWR_ON is set.
  reg(0x05000000) = 0xa630;
  reg(0x05090000 + 0x100c) = 0;                    // gx off
  assert(t.read32(nullptr, 0x05000000, v, 1) == kErrDenied && native_dwc3_mmio_refused == 1);
  reg(0x05090000 + 0x100c) = 0x80000000u;          // gx on
  assert(t.read32(nullptr, 0x05000000, v, 1) == kOk && v[0] == 0xa630);
  // The GMU follows gpu_cx; GPUCC itself is never guarded.
  reg(0x05090000 + 0x106c) = 0;
  assert(t.read32(nullptr, 0x0506a000, v, 1) == kErrDenied);
  assert(t.read32(nullptr, 0x05090000 + 0x106c, v, 1) == kOk);
  // A read that crosses into a guarded block stops there.
  reg(0x00100000 + 0x77004) = 0;                   // ufs off
  assert(t.read32(nullptr, 0x01d83ffc, v, 2) == kErrDenied);

  // DRAM never goes through read32; physical reads are bounded to managed RAM.
  assert(t.read32(nullptr, 0x80000000ull, v, 1) == kErrArgs);
  gRam[5] = 0xab;
  uint8_t buf[8] = {};
  assert(t.readPhys(nullptr, 0x80000004ull, buf, 4) == kOk && buf[1] == 0xab);
  assert(t.readPhys(nullptr, 0x80000000ull + gRam.size() - 2, buf, 4) == kErrArgs);   // runs past RAM
  assert(t.readPhys(nullptr, 0x03400000, buf, 4) == kErrArgs);                        // MMIO is read32's

  // Writes: refused by the command layer unless usb-debug>=2 (allowWrites).
  putLe16(req, kOpWrite32); putLe16(req + 2, 0); putLe64(req + 4, 0x03400020); putLe32(req + 12, 7);
  handleCommand(t, req, 16, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kErrDenied && reg(0x03400020) == 0);
  usbdbgk::fill(t, true);
  handleCommand(t, req, 16, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kOk && reg(0x03400020) == 7);

  // readVirt: an unmapped address answers kErrFault instead of faulting.
  assert(t.readVirt(nullptr, 0, buf, 4) == kErrFault);

  // kcmd: help, the built-in power listing, a registered command, an unknown one.
  char out[2048];
  size_t n = t.kcmd(nullptr, "help", 4, out, sizeof out);
  assert(n && strstr(out, "power"));
  assert(fajita_usbdbg_register_kcmd("echo", echoCmd, "say it back") == 0);
  n = t.kcmd(nullptr, "help", 4, out, sizeof out);
  out[n] = 0;
  assert(strstr(out, "echo  say it back"));
  n = t.kcmd(nullptr, "echo  hi there", 14, out, sizeof out);
  out[n] = 0;
  assert(!strcmp(out, "echo:hi there") && native_dwc3_kcmds == 1);
  n = t.kcmd(nullptr, "power", 5, out, sizeof out);
  out[n] = 0;
  assert(strstr(out, "gpu (gx)") && strstr(out, " on\n") && strstr(out, "ufs") && strstr(out, "OFF"));
  n = t.kcmd(nullptr, "nosuch", 6, out, sizeof out);
  out[n] = 0;
  assert(strstr(out, "unknown command 'nosuch'"));

  // reset: refused without writes, accepted with them, carried out by the poll thread after the reply.
  putLe16(req, kOpReset); putLe16(req + 2, 0); putLe32(req + 4, 0);
  usbdbgk::fill(t, false);
  handleCommand(t, req, 8, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kErrDenied && !gResets);
  usbdbgk::fill(t, true);
  handleCommand(t, req, 8, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kOk && !gResets);
  for (int i = 0; i < 40; i++) usbdbgk::pollReset();
  assert(gResets == 1);

  printf("KernelTarget: MMIO power guard, RAM bound, write gate and kcmd assertions passed\n");
  return 0;
}
