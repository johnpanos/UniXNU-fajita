// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the CDC-ACM descriptors (AcmDescriptors.h) and the ep0 request handler
// (AcmEp0.h): it walks a host's enumeration and checks the bytes and actions.
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../AcmEp0.h"

using namespace acmep0;

static Setup su(uint8_t rt, uint8_t rq, uint16_t val, uint16_t idx, uint16_t len)
{
  return Setup{rt, rq, val, idx, len};
}

int main()
{
  // Device descriptor: length, type, class 02 (CDC), ep0 max 64, our IDs.
  uint8_t dev[18];
  acmdesc::device(dev);
  assert(dev[0] == 18 && dev[1] == 1 && dev[4] == 0x02 && dev[7] == 64);
  assert((dev[8] | (dev[9] << 8)) == acmdesc::kVendor && (dev[10] | (dev[11] << 8)) == acmdesc::kProduct);

  // Config: wTotalLength equals the emitted length, 2 interfaces, and the three
  // endpoints with the high-speed bulk max of 512.
  uint8_t cfg[acmdesc::kConfigLen];
  acmdesc::config(cfg, 512, 2);
  assert(cfg[0] == 9 && cfg[1] == 2 && (cfg[2] | (cfg[3] << 8)) == acmdesc::kConfigLen && cfg[4] == 2);
  unsigned eps = 0, off = 0;
  bool sawBulkIn = false, sawBulkOut = false, sawNotify = false;
  while (off + 2 <= sizeof cfg) {
    uint8_t blen = cfg[off], btype = cfg[off + 1];
    assert(blen >= 2 && off + blen <= sizeof cfg);
    if (btype == 5) {
      eps++;
      uint8_t addr = cfg[off + 2], attr = cfg[off + 3];
      uint16_t mp = cfg[off + 4] | (cfg[off + 5] << 8);
      if (addr == acmdesc::kEpBulkIn) { sawBulkIn = true; assert(attr == 2 && mp == 512); }
      if (addr == acmdesc::kEpBulkOut) { sawBulkOut = true; assert(attr == 2 && mp == 512); }
      if (addr == acmdesc::kEpNotify) { sawNotify = true; assert(attr == 3); }
    }
    off += blen;
  }
  assert(off == sizeof cfg && eps == 3 && sawBulkIn && sawBulkOut && sawNotify);

  // Full-speed config reports 64-byte bulk; the DWC3 physical ep indices are right.
  acmdesc::config(cfg, 64, 2);
  assert((cfg[acmdesc::kConfigLen - 3] | (cfg[acmdesc::kConfigLen - 2] << 8)) == 64);
  assert(acmdesc::physical(acmdesc::kEpBulkOut) == 4 && acmdesc::physical(acmdesc::kEpBulkIn) == 5);
  assert(acmdesc::physical(acmdesc::kEpNotify) == 3 && acmdesc::physical(0x00) == 0 && acmdesc::physical(0x80) == 1);

  // String 0 is the language list; string 2 is UTF-16LE text of the right length.
  uint8_t str[256];
  assert(acmdesc::string(0, str, sizeof str) == 4 && str[2] == 0x09 && str[3] == 0x04);
  size_t sn = acmdesc::string(2, str, sizeof str);
  assert(sn == 2 + 2 * strlen("fajita usb-debug") && str[1] == 3 && str[2] == 'f' && str[3] == 0);
  assert(acmdesc::string(9, str, sizeof str) == 0);   // out of range

  State st;
  // GET_DESCRIPTOR(device): returns the 18-byte device descriptor, capped at wLength.
  Ep0Action a = handleSetup(st, su(0x80, kGetDescriptor, 0x0100, 0, 64));
  assert(a.kind == kTxData && a.len == 18 && a.buf[1] == 1);
  a = handleSetup(st, su(0x80, kGetDescriptor, 0x0100, 0, 8));
  assert(a.kind == kTxData && a.len == 8);                 // the host's first 8-byte probe

  // GET_DESCRIPTOR(config) returns wTotalLength bytes.
  a = handleSetup(st, su(0x80, kGetDescriptor, 0x0200, 0, 255));
  assert(a.kind == kTxData && a.len == acmdesc::kConfigLen && a.buf[1] == 2);

  // SET_ADDRESS / SET_CONFIGURATION carry their value to the controller.
  a = handleSetup(st, su(0x00, kSetAddress, 42, 0, 0));
  assert(a.kind == kSetAddressAck && a.value == 42);
  a = handleSetup(st, su(0x00, kSetConfiguration, 1, 0, 0));
  assert(a.kind == kSetConfigAck && a.value == 1);

  // CDC line coding: GET returns 7 bytes (115200 = 0x1C200 little-endian), SET is accepted.
  a = handleSetup(st, su(0xA1, kCdcGetLineCoding, 0, 0, 7));
  assert(a.kind == kTxData && a.len == 7 && a.buf[0] == 0x00 && a.buf[1] == 0xC2 && a.buf[2] == 0x01 && a.buf[6] == 8);
  a = handleSetup(st, su(0x21, kCdcSetLineCoding, 0, 0, 7));
  assert(a.kind == kRxData && a.len == 7);
  a = handleSetup(st, su(0x21, kCdcSetControlLineState, 0x03, 0, 0));
  assert(a.kind == kStatusOnly);

  // GET_STATUS(device) = self-powered; an unknown vendor request stalls.
  a = handleSetup(st, su(0x80, kGetStatus, 0, 0, 2));
  assert(a.kind == kTxData && a.len == 2 && a.buf[0] == 0x01);
  a = handleSetup(st, su(0xC0, 0x99, 0, 0, 4));
  assert(a.kind == kStall);

  // Full-speed state reports the other-speed config at the opposite bulk max.
  st.highSpeed = false;
  a = handleSetup(st, su(0x80, kGetDescriptor, 0x0700, 0, 255));
  assert(a.kind == kTxData && a.buf[1] == kDescOtherSpeed &&
         (a.buf[acmdesc::kConfigLen - 3] | (a.buf[acmdesc::kConfigLen - 2] << 8)) == 512);

  printf("AcmEnum: descriptor and ep0 enumeration assertions passed\n");
  return 0;
}
