// SPDX-License-Identifier: BSD-2-Clause-Patent
// Kernel hooks of the fjdbg command layer (UsbDbgDevice.h Target): memory and MMIO peek/poke and named kernel commands.
//
//   read32/write32  device MMIO: each 4 KiB page is mapped once with ml_io_map (Device-nGnRE) and kept in a small table
//                   for the driver's life, so poking a register block costs one map. An access to a block whose clock or
//                   power domain is off can stall the interconnect: that is the caller's risk (the watchdog is the backstop).
//   readPhys        ml_phys_read_word / _byte (DRAM through the physical aperture, anything else through a copy window)
//   readVirt        ml_nofault_copy (a kernel address that is not mapped returns kErrFault, it never faults)
//   writePhys       ml_phys_write_word / _byte
//   kcmd            "help" lists the commands; every other first word is looked up in the table that drivers fill with
//                   fajita_usbdbg_register_kcmd (weak in the callers, so a kernel without NativeDwc3 still links). A
//                   handler runs on the USB poll thread: it must return quickly and start a thread for long work.
//
// Writes need usb-debug>=2 (the Target's allowWrites). Included once, by Dwc3GadgetRun.h in NativeDwc3.cpp.
#ifndef NATIVE_DWC3_USBDBG_KERNEL_TARGET_H
#define NATIVE_DWC3_USBDBG_KERNEL_TARGET_H

#include "UsbDbgDevice.h"

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
unsigned int ml_phys_read_word(vm_offset_t paddr);
unsigned int ml_phys_read_byte(vm_offset_t paddr);
void ml_phys_write_word(vm_offset_t paddr, unsigned int data);
void ml_phys_write_byte(vm_offset_t paddr, unsigned int data);
vm_size_t ml_nofault_copy(vm_offset_t virtsrc, vm_offset_t virtdst, vm_size_t size);
typedef size_t (*fajita_usbdbg_kcmd_fn)(const char *args, size_t len, char *out, size_t cap);
int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help);
extern volatile uint32_t native_dwc3_gadget_on;   // NativeDwc3.cpp: the usb-debug value
volatile uint32_t native_dwc3_mmio_maps = 0, native_dwc3_kcmds = 0;
}

namespace usbdbgk {

struct MmioPage { uint64_t pa; vm_offset_t va; };
enum { kMmioPages = 128, kKcmds = 16 };
static MmioPage gMmio[kMmioPages];
static unsigned gMmioCount;

struct Kcmd { const char *name; fajita_usbdbg_kcmd_fn fn; const char *help; };
static Kcmd gKcmd[kKcmds];
static volatile unsigned gKcmdCount;

// Callers are the poll thread only (one command at a time), so the table needs no lock.
static volatile uint32_t *mmioWord(uint64_t pa)
{
  const uint64_t page = pa & ~uint64_t(0xfff);
  for (unsigned i = 0; i < gMmioCount; i++)
    if (gMmio[i].pa == page) return reinterpret_cast<volatile uint32_t *>(gMmio[i].va + (pa & 0xfff));
  if (gMmioCount >= kMmioPages) return nullptr;
  vm_offset_t va = ml_io_map(vm_offset_t(page), 0x1000);
  if (!va) return nullptr;
  gMmio[gMmioCount++] = MmioPage{page, va};
  native_dwc3_mmio_maps = gMmioCount;
  return reinterpret_cast<volatile uint32_t *>(va + (pa & 0xfff));
}

// RAM of the physical aperture is never device memory: refuse ml_io_map of it (an aliased Device mapping of normal
// memory is unpredictable). The SDM845 DRAM starts at 0x80000000; every block a register peek wants is below it.
static bool isMmio(uint64_t pa) { return pa < 0x80000000ull; }

static int32_t read32(void *, uint64_t pa, uint32_t *out, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) {
    uint64_t a = pa + 4ull * i;
    if (!isMmio(a)) return usbdbg::kErrArgs;
    volatile uint32_t *w = mmioWord(a);
    if (!w) return usbdbg::kErrFault;
    out[i] = *w;
  }
  return usbdbg::kOk;
}

static int32_t write32(void *, uint64_t pa, const uint32_t *in, uint32_t count)
{
  for (uint32_t i = 0; i < count; i++) {
    uint64_t a = pa + 4ull * i;
    if (!isMmio(a)) return usbdbg::kErrArgs;
    volatile uint32_t *w = mmioWord(a);
    if (!w) return usbdbg::kErrFault;
    *w = in[i];
    __asm__ volatile("dsb sy" ::: "memory");
  }
  return usbdbg::kOk;
}

static int32_t readPhys(void *, uint64_t pa, void *out, uint32_t len)
{
  if (pa < 0x80000000ull) return usbdbg::kErrArgs;   // MMIO goes through read32
  uint8_t *o = static_cast<uint8_t *>(out);
  uint32_t i = 0;
  while (i < len) {
    uint64_t a = pa + i;
    if (!(a & 3) && len - i >= 4) {
      uint32_t v = ml_phys_read_word(vm_offset_t(a));
      __builtin_memcpy(o + i, &v, 4);
      i += 4;
    } else {
      o[i] = uint8_t(ml_phys_read_byte(vm_offset_t(a)));
      i++;
    }
  }
  return usbdbg::kOk;
}

static int32_t writePhys(void *, uint64_t pa, const void *in, uint32_t len)
{
  if (pa < 0x80000000ull) return usbdbg::kErrArgs;
  const uint8_t *s = static_cast<const uint8_t *>(in);
  uint32_t i = 0;
  while (i < len) {
    uint64_t a = pa + i;
    if (!(a & 3) && len - i >= 4) {
      uint32_t v;
      __builtin_memcpy(&v, s + i, 4);
      ml_phys_write_word(vm_offset_t(a), v);
      i += 4;
    } else {
      ml_phys_write_byte(vm_offset_t(a), s[i]);
      i++;
    }
  }
  return usbdbg::kOk;
}

static int32_t readVirt(void *, uint64_t kva, void *out, uint32_t len)
{
  vm_size_t n = ml_nofault_copy(vm_offset_t(kva), reinterpret_cast<vm_offset_t>(out), len);
  return n == len ? usbdbg::kOk : usbdbg::kErrFault;
}

static int32_t writeVirt(void *, uint64_t kva, const void *in, uint32_t len)
{
  vm_size_t n = ml_nofault_copy(reinterpret_cast<vm_offset_t>(in), vm_offset_t(kva), len);
  return n == len ? usbdbg::kOk : usbdbg::kErrFault;
}

static size_t kcmd(void *, const char *cmd, size_t len, char *out, size_t cap)
{
  size_t w = 0;
  while (len && (cmd[0] == ' ' || cmd[0] == '\0')) { cmd++; len--; }
  size_t nameLen = 0;
  while (nameLen < len && cmd[nameLen] != ' ' && cmd[nameLen] != '\0') nameLen++;
  const char *args = cmd + nameLen;
  size_t argsLen = len - nameLen;
  while (argsLen && args[0] == ' ') { args++; argsLen--; }
  const unsigned n = gKcmdCount;
  if (nameLen == 0 || (nameLen == 4 && !__builtin_memcmp(cmd, "help", 4))) {
    for (unsigned i = 0; i < n && w < cap; i++) {
      int k = snprintf(out + w, cap - w, "%s  %s\n", gKcmd[i].name, gKcmd[i].help ? gKcmd[i].help : "");
      if (k < 0) break;
      w += size_t(k) < cap - w ? size_t(k) : cap - w;
    }
    if (!n && cap) { int k = snprintf(out, cap, "no kernel commands registered\n"); w = k > 0 ? size_t(k) : 0; }
    return w < cap ? w : cap;
  }
  for (unsigned i = 0; i < n; i++) {
    const char *name = gKcmd[i].name;
    size_t l = 0;
    while (name[l]) l++;
    if (l == nameLen && !__builtin_memcmp(name, cmd, l)) {
      native_dwc3_kcmds = native_dwc3_kcmds + 1;
      size_t r = gKcmd[i].fn(args, argsLen, out, cap);
      return r < cap ? r : cap;
    }
  }
  int k = snprintf(out, cap, "unknown command '%.*s' (try help)\n", int(nameLen), cmd);
  return k > 0 ? (size_t(k) < cap ? size_t(k) : cap) : 0;
}

static void fill(usbdbg::Target &t, bool writes)
{
  t.features |= usbdbg::kFeatMemory | usbdbg::kFeatMmio | usbdbg::kFeatKcmd;
  t.allowWrites = writes;
  t.read32 = read32;
  t.write32 = write32;
  t.readPhys = readPhys;
  t.writePhys = writePhys;
  t.readVirt = readVirt;
  t.writeVirt = writeVirt;
  t.kcmd = kcmd;
}

}  // namespace usbdbgk

// Registration is open to any driver from any thread before or after the gadget runs; a slot is filled before the count
// that publishes it, so the poll thread never sees a half-written entry.
extern "C" int fajita_usbdbg_register_kcmd(const char *name, fajita_usbdbg_kcmd_fn fn, const char *help)
{
  static volatile uint32_t claim;
  if (!name || !fn) return -1;
  uint32_t slot = __atomic_fetch_add(&claim, 1, __ATOMIC_ACQ_REL);
  if (slot >= usbdbgk::kKcmds) return -1;
  usbdbgk::gKcmd[slot] = usbdbgk::Kcmd{name, fn, help};
  __atomic_thread_fence(__ATOMIC_RELEASE);
  while (usbdbgk::gKcmdCount != slot) __asm__ volatile("yield");   // publish in claim order
  usbdbgk::gKcmdCount = slot + 1;
  return 0;
}

#endif
