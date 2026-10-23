// SPDX-License-Identifier: BSD-2-Clause-Patent
// USB descriptors of the NativeDwc3 debug gadget: one CDC-ACM function (two
// interfaces: communication + data). Field layouts are those of USB 2.0 ch. 9 and
// CDC 1.2 (as include/uapi/linux/usb/ch9.h and cdc.h record them).
//
// The communication interface declares protocol 0 (no AT commands), so the host's
// ModemManager does not probe the port; Linux cdc_acm binds it as /dev/ttyACM*.
// IDs: 1209:4d69. systemd's 70-analyzers.hwdb tags that ID ID_SIGNAL_ANALYZER, so
// logind gives the seat user an ACL on the tty (no root, no udev rule needed).
#ifndef NATIVE_DWC3_ACM_DESCRIPTORS_H
#define NATIVE_DWC3_ACM_DESCRIPTORS_H

#include <stddef.h>
#include <stdint.h>

namespace acmdesc {

enum : uint16_t { kVendor = 0x1209, kProduct = 0x4d69, kRelease = 0x0100 };
enum : uint8_t { kEpNotify = 0x81, kEpBulkOut = 0x02, kEpBulkIn = 0x82 };
enum : size_t { kDeviceLen = 18, kQualifierLen = 10, kConfigLen = 67 };

// DWC3 physical endpoint index: (number << 1) | direction-in.
static inline unsigned physical(uint8_t address) { return ((address & 0x0fu) << 1) | ((address & 0x80u) ? 1u : 0u); }

static inline void device(uint8_t *d)
{
  const uint8_t v[kDeviceLen] = {18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
                                 uint8_t(kVendor), uint8_t(kVendor >> 8), uint8_t(kProduct), uint8_t(kProduct >> 8),
                                 uint8_t(kRelease), uint8_t(kRelease >> 8), 1, 2, 3, 1};
  for (size_t i = 0; i < kDeviceLen; i++) d[i] = v[i];
}

static inline void qualifier(uint8_t *d)
{
  const uint8_t v[kQualifierLen] = {10, 6, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 1, 0};
  for (size_t i = 0; i < kQualifierLen; i++) d[i] = v[i];
}

// The configuration. `bulkMax` is 512 at high speed, 64 at full speed; `type` is 2
// (configuration) or 7 (other-speed configuration).
static inline void config(uint8_t *d, uint16_t bulkMax, uint8_t type)
{
  const uint8_t lo = uint8_t(bulkMax), hi = uint8_t(bulkMax >> 8);
  const uint8_t v[kConfigLen] = {
    9, type, kConfigLen, 0, 2, 1, 0, 0xC0, 1,          // config: 2 interfaces, self-powered, 2 mA
    9, 4, 0, 0, 1, 0x02, 0x02, 0x00, 0,                // interface 0: CDC comm, ACM, protocol 0
    5, 0x24, 0x00, 0x10, 0x01,                         // CDC header 1.10
    5, 0x24, 0x01, 0x00, 1,                            // call management: data interface 1
    4, 0x24, 0x02, 0x02,                               // ACM: line coding + serial state
    5, 0x24, 0x06, 0, 1,                               // union: master 0, slave 1
    7, 5, kEpNotify, 0x03, 16, 0, 9,                   // notification: interrupt IN 16 B
    9, 4, 1, 0, 2, 0x0A, 0x00, 0x00, 0,                // interface 1: CDC data
    7, 5, kEpBulkOut, 0x02, lo, hi, 0,                 // bulk OUT
    7, 5, kEpBulkIn, 0x02, lo, hi, 0,                  // bulk IN
  };
  for (size_t i = 0; i < kConfigLen; i++) d[i] = v[i];
}

// String descriptor `index` (0 = language list) as UTF-16LE; returns its length.
static inline size_t string(uint8_t index, uint8_t *d, size_t cap)
{
  static const char *const kText[] = {nullptr, "UniXNU", "fajita usb-debug", "fajita0"};
  if (cap < 4) return 0;
  if (index == 0) { d[0] = 4; d[1] = 3; d[2] = 0x09; d[3] = 0x04; return 4; }
  if (index >= sizeof kText / sizeof kText[0]) return 0;
  size_t n = 2;
  for (const char *s = kText[index]; *s && n + 2 <= cap && n + 2 <= 255; s++) { d[n++] = uint8_t(*s); d[n++] = 0; }
  d[0] = uint8_t(n);
  d[1] = 3;
  return n;
}

}  // namespace acmdesc

#endif
