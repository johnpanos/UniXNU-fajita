// SPDX-License-Identifier: BSD-2-Clause-Patent
// fjdbg command layer: decodes one CMD-channel request and builds its RSP reply.
//
// Request data:  u16 op, u16 flags (0), op-specific arguments (little-endian)
// Reply data:    u16 op | 0x8000, u16 0, i32 status, op-specific result
//
// Everything that touches the machine goes through a Target table, so the same
// dispatcher runs in the kernel (NativeDwc3 fills the table with ml_nofault_copy,
// physical-copy and ml_io_map backed helpers, the msgbuf, BootReset and the ArmCpu
// watchdog) and in the host simulator (tests/UsbDbgSim.cpp, a fake memory), which
// scripts/fajita-usbdbg.py drives end to end in the host tests.
//
// Writes (memory, MMIO), panic and reset are refused with kErrDenied unless the
// target says allowWrites (usb-debug=2 in the kernel): a read-only session cannot
// change the machine. Freestanding, like UsbDbgProto.h.
#ifndef NATIVE_DWC3_USBDBG_DEVICE_H
#define NATIVE_DWC3_USBDBG_DEVICE_H

#include "UsbDbgProto.h"

namespace usbdbg {

enum Op : uint16_t {
  kOpHello = 0x0001,      // -> u8 version, u8 0, u16 maxData, u32 features, ident text
  kOpState = 0x0002,      // -> "key=value\n" text (driver counters, link state, watchdog)
  kOpReadVirt = 0x0010,   // u64 kva, u32 len            -> bytes (fault-safe copy)
  kOpWriteVirt = 0x0011,  // u64 kva, bytes              -> (none)            [write]
  kOpReadPhys = 0x0012,   // u64 pa, u32 len             -> bytes
  kOpWritePhys = 0x0013,  // u64 pa, bytes               -> (none)            [write]
  kOpRead32 = 0x0014,     // u64 pa, u32 count           -> count x u32 (device MMIO)
  kOpWrite32 = 0x0015,    // u64 pa, u32 value...        -> (none)            [write]
  kOpLogFrom = 0x0020,    // u64 offset                  -> u64 offset actually used
  kOpKcmd = 0x0030,       // command text                -> text (named kernel debug commands)
  kOpPanic = 0x0040,      // message text                -> does not return   [write]
  kOpReset = 0x0041,      // u32 mode (0 bootloader)     -> (none)            [write]
  kOpWatchdog = 0x0042,   // u32 action (0 stop petting) -> (none)            [write]
  kOpReplyBit = 0x8000,
};

enum Status : int32_t {
  kOk = 0,
  kErrOp = -1,       // unknown op
  kErrArgs = -2,     // short/malformed arguments
  kErrFault = -3,    // the access faulted or the address is not mapped
  kErrDenied = -4,   // a write on a read-only session
  kErrTooBig = -5,   // the result would not fit one frame
  kErrUnsupported = -6,
};

enum Feature : uint32_t {
  kFeatLog = 1u << 0,
  kFeatMemory = 1u << 1,
  kFeatMmio = 1u << 2,
  kFeatKcmd = 1u << 3,
  kFeatWrites = 1u << 4,
  kFeatKdp = 1u << 5,
};

// kMaxWrite32 bounds the on-stack copy of a Write32 request (the panic-path stack is small).
enum : size_t { kReplyHeader = 8, kMaxReplyData = kMaxData - kReplyHeader, kMaxWrite32 = 64 };

// Every hook may be null (the op then answers kErrUnsupported). Memory hooks return
// a Status; text hooks return the number of bytes written into `out`.
struct Target {
  void *ctx;
  const char *ident;
  bool allowWrites;
  uint32_t features;
  int32_t (*readVirt)(void *ctx, uint64_t kva, void *out, uint32_t len);
  int32_t (*writeVirt)(void *ctx, uint64_t kva, const void *in, uint32_t len);
  int32_t (*readPhys)(void *ctx, uint64_t pa, void *out, uint32_t len);
  int32_t (*writePhys)(void *ctx, uint64_t pa, const void *in, uint32_t len);
  int32_t (*read32)(void *ctx, uint64_t pa, uint32_t *out, uint32_t count);
  int32_t (*write32)(void *ctx, uint64_t pa, const uint32_t *in, uint32_t count);
  size_t (*state)(void *ctx, char *out, size_t cap);
  size_t (*kcmd)(void *ctx, const char *cmd, size_t len, char *out, size_t cap);
  int32_t (*logFrom)(void *ctx, uint64_t offset, uint64_t *used);
  int32_t (*raisePanic)(void *ctx, const char *msg, size_t len);
  int32_t (*reset)(void *ctx, uint32_t mode);
  int32_t (*watchdog)(void *ctx, uint32_t action);
};

// Handle one request; writes the reply data (header + result) into `rsp` and returns
// its length. `rsp` must hold kMaxData bytes and be 4-byte aligned. Never fails to
// produce a reply.
static inline size_t handleCommand(const Target &t, const uint8_t *req, size_t len, uint8_t *rsp)
{
  uint16_t op = len >= 2 ? getLe16(req) : 0;
  const uint8_t *arg = req + 4;
  size_t argLen = len >= 4 ? len - 4 : 0;
  uint8_t *out = rsp + kReplyHeader;
  size_t outLen = 0;
  int32_t status = kOk;

  auto finish = [&](int32_t st, size_t n) -> size_t {
    putLe16(rsp, uint16_t(op | kOpReplyBit));
    putLe16(rsp + 2, 0);
    putLe32(rsp + 4, uint32_t(st));
    return kReplyHeader + (st == kOk ? n : 0);
  };
  if (len < 4) return finish(kErrArgs, 0);
  bool writeOp = op == kOpWriteVirt || op == kOpWritePhys || op == kOpWrite32 || op == kOpPanic || op == kOpReset ||
                 op == kOpWatchdog;
  if (writeOp && !t.allowWrites) return finish(kErrDenied, 0);

  switch (op) {
  case kOpHello: {
    out[0] = kProtoVersion;
    out[1] = 0;
    putLe16(out + 2, uint16_t(kMaxData));
    putLe32(out + 4, t.features | (t.allowWrites ? uint32_t(kFeatWrites) : 0));
    outLen = 8;
    for (const char *s = t.ident ? t.ident : ""; *s && outLen < 72; s++) out[outLen++] = uint8_t(*s);
    break;
  }
  case kOpState:
    if (!t.state) return finish(kErrUnsupported, 0);
    outLen = t.state(t.ctx, reinterpret_cast<char *>(out), kMaxReplyData);
    break;
  case kOpReadVirt:
  case kOpReadPhys: {
    if (argLen != 12) return finish(kErrArgs, 0);
    uint64_t addr = getLe64(arg);
    uint32_t n = getLe32(arg + 8);
    if (n > kMaxReplyData) return finish(kErrTooBig, 0);
    auto fn = op == kOpReadVirt ? t.readVirt : t.readPhys;
    if (!fn) return finish(kErrUnsupported, 0);
    status = fn(t.ctx, addr, out, n);
    outLen = n;
    break;
  }
  case kOpWriteVirt:
  case kOpWritePhys: {
    if (argLen < 8) return finish(kErrArgs, 0);
    auto fn = op == kOpWriteVirt ? t.writeVirt : t.writePhys;
    if (!fn) return finish(kErrUnsupported, 0);
    status = fn(t.ctx, getLe64(arg), arg + 8, uint32_t(argLen - 8));
    break;
  }
  case kOpRead32: {
    if (argLen != 12) return finish(kErrArgs, 0);
    uint64_t pa = getLe64(arg);
    uint32_t count = getLe32(arg + 8);
    if ((pa & 3) || count == 0) return finish(kErrArgs, 0);
    if (count > kMaxReplyData / 4) return finish(kErrTooBig, 0);
    if (!t.read32) return finish(kErrUnsupported, 0);
    // `rsp` is 4-byte aligned (see below), so the words land in place; then make
    // them little-endian in place (a no-op on arm64 and x86, kept for clarity).
    uint32_t *words = reinterpret_cast<uint32_t *>(out);
    status = t.read32(t.ctx, pa, words, count);
    for (uint32_t i = 0; i < count; i++) { uint32_t v = words[i]; putLe32(out + 4 * i, v); }
    outLen = 4 * size_t(count);
    break;
  }
  case kOpWrite32: {
    if (argLen < 12 || ((argLen - 8) & 3) || argLen - 8 > 4 * kMaxWrite32) return finish(kErrArgs, 0);
    uint64_t pa = getLe64(arg);
    if (pa & 3) return finish(kErrArgs, 0);
    if (!t.write32) return finish(kErrUnsupported, 0);
    uint32_t count = uint32_t((argLen - 8) / 4);
    uint32_t words[kMaxWrite32];
    for (uint32_t i = 0; i < count; i++) words[i] = getLe32(arg + 8 + 4 * i);
    status = t.write32(t.ctx, pa, words, count);
    break;
  }
  case kOpLogFrom: {
    if (argLen != 8) return finish(kErrArgs, 0);
    if (!t.logFrom) return finish(kErrUnsupported, 0);
    uint64_t used = 0;
    status = t.logFrom(t.ctx, getLe64(arg), &used);
    putLe64(out, used);
    outLen = 8;
    break;
  }
  case kOpKcmd:
    if (!t.kcmd) return finish(kErrUnsupported, 0);
    outLen = t.kcmd(t.ctx, reinterpret_cast<const char *>(arg), argLen, reinterpret_cast<char *>(out), kMaxReplyData);
    break;
  case kOpPanic:
    if (!t.raisePanic) return finish(kErrUnsupported, 0);
    status = t.raisePanic(t.ctx, reinterpret_cast<const char *>(arg), argLen);
    break;
  case kOpReset:
  case kOpWatchdog: {
    if (argLen != 4) return finish(kErrArgs, 0);
    auto fn = op == kOpReset ? t.reset : t.watchdog;
    if (!fn) return finish(kErrUnsupported, 0);
    status = fn(t.ctx, getLe32(arg));
    break;
  }
  default:
    return finish(kErrOp, 0);
  }
  if (outLen > kMaxReplyData) return finish(kErrTooBig, 0);
  return finish(status, outLen);
}

// Build a LOG-channel frame's data: u64 stream offset, then the text bytes.
static inline size_t buildLogData(uint64_t offset, const uint8_t *text, size_t len, uint8_t *out)
{
  if (len > kMaxData - 8) len = kMaxData - 8;
  putLe64(out, offset);
  for (size_t i = 0; i < len; i++) out[8 + i] = text[i];
  return 8 + len;
}

}  // namespace usbdbg

#endif
