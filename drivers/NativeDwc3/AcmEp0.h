// SPDX-License-Identifier: BSD-2-Clause-Patent
// Endpoint-0 request handling for the CDC-ACM debug gadget, as pure logic: a SETUP
// packet plus the current gadget state in, an Ep0Action out. The controller code
// (NativeDwc3.cpp) carries the action out on the DWC3 ep0 TRBs; this file touches no
// hardware, so the host tests drive the whole standard/CDC enumeration offline.
//
// Standard device/interface requests (USB 2.0 §9.4) and the three CDC-ACM class
// requests (SET/GET_LINE_CODING, SET_CONTROL_LINE_STATE) the host sends. Anything
// else is stalled. Follows the control flow of dwc3/ep0.c without copying it.
#ifndef NATIVE_DWC3_ACM_EP0_H
#define NATIVE_DWC3_ACM_EP0_H

#include "AcmDescriptors.h"

namespace acmep0 {

struct Setup {                 // the 8 SETUP bytes, little-endian
  uint8_t bmRequestType, bRequest;
  uint16_t wValue, wIndex, wLength;
};
static inline Setup parseSetup(const uint8_t *p)
{
  return Setup{p[0], p[1], uint16_t(p[2] | (p[3] << 8)), uint16_t(p[4] | (p[5] << 8)), uint16_t(p[6] | (p[7] << 8))};
}

// Standard and class request codes.
enum : uint8_t {
  kGetStatus = 0, kClearFeature = 1, kSetFeature = 3, kSetAddress = 5, kGetDescriptor = 6,
  kSetConfiguration = 9, kGetConfiguration = 8, kSetInterface = 11, kGetInterface = 10,
  kCdcSetLineCoding = 0x20, kCdcGetLineCoding = 0x21, kCdcSetControlLineState = 0x22,
};
enum : uint8_t { kDescDevice = 1, kDescConfig = 2, kDescString = 3, kDescQualifier = 6, kDescOtherSpeed = 7 };

enum Kind : uint8_t {
  kStall,          // unsupported: stall ep0
  kTxData,         // IN data stage then OUT status: send `len` bytes from `buf`
  kRxData,         // OUT data stage then IN status: receive `len` bytes (then accept)
  kStatusOnly,     // no data stage: just the status phase (ack)
  kSetAddressAck,  // like kStatusOnly, but program DCFG address = value after status
  kSetConfigAck,   // like kStatusOnly, but enable the data endpoints (value != 0)
};

struct Ep0Action {
  Kind kind;
  uint16_t len;        // bytes for kTxData / kRxData
  uint8_t value;       // address (kSetAddressAck) or configuration (kSetConfigAck)
  uint8_t buf[256];    // descriptor bytes for kTxData
};

struct State {
  bool highSpeed = true;   // DSTS connect speed decides bulk max and which config shape to report
  uint8_t config = 0;
};

static inline uint16_t bulkMax(bool highSpeed) { return highSpeed ? 512 : 64; }

// Build the response to one SETUP packet. Line-coding GETs return a fixed 7-byte
// record; SETs are accepted and ignored (a kernel debug tty has no real UART).
static inline Ep0Action handleSetup(const State &st, const Setup &s)
{
  Ep0Action a{};
  const bool deviceToHost = (s.bmRequestType & 0x80) != 0;
  const uint8_t type = (s.bmRequestType >> 5) & 3;   // 0 standard, 1 class

  if (type == 1) {   // CDC-ACM class, interface recipient
    switch (s.bRequest) {
    case kCdcGetLineCoding: {
      const uint8_t coding[7] = {0x00, 0xC2, 0x01, 0x00, 0, 0, 8};   // 115200 8N1
      a.kind = kTxData;
      a.len = s.wLength < 7 ? s.wLength : 7;
      for (unsigned i = 0; i < a.len; i++) a.buf[i] = coding[i];
      return a;
    }
    case kCdcSetLineCoding:
      a.kind = s.wLength ? kRxData : kStatusOnly;
      a.len = s.wLength;
      return a;
    case kCdcSetControlLineState:
      a.kind = kStatusOnly;
      return a;
    default:
      a.kind = kStall;
      return a;
    }
  }

  if (type != 0) { a.kind = kStall; return a; }   // vendor requests: none

  switch (s.bRequest) {
  case kGetDescriptor: {
    const uint8_t descType = uint8_t(s.wValue >> 8), descIndex = uint8_t(s.wValue);
    a.kind = kTxData;
    size_t n = 0;
    if (descType == kDescDevice) {
      acmdesc::device(a.buf);
      n = acmdesc::kDeviceLen;
    } else if (descType == kDescConfig || descType == kDescOtherSpeed) {
      // Our config is reported for the speed we are at; the other-speed config mirrors it
      // at the opposite bulk max so a host that asks can switch cleanly.
      bool hs = descType == kDescConfig ? st.highSpeed : !st.highSpeed;
      acmdesc::config(a.buf, bulkMax(hs), descType);
      n = acmdesc::kConfigLen;
    } else if (descType == kDescQualifier) {
      acmdesc::qualifier(a.buf);
      n = acmdesc::kQualifierLen;
    } else if (descType == kDescString) {
      n = acmdesc::string(descIndex, a.buf, sizeof a.buf);
      if (!n) { a.kind = kStall; return a; }
    } else {
      a.kind = kStall;
      return a;
    }
    a.len = uint16_t(n < s.wLength ? n : s.wLength);   // never return more than asked
    return a;
  }
  case kSetAddress:
    a.kind = kSetAddressAck;
    a.value = uint8_t(s.wValue);
    return a;
  case kSetConfiguration:
    a.kind = kSetConfigAck;
    a.value = uint8_t(s.wValue);
    return a;
  case kGetConfiguration:
    a.kind = kTxData;
    a.len = s.wLength ? 1 : 0;
    a.buf[0] = st.config;
    return a;
  case kGetStatus:
    a.kind = kTxData;
    a.len = s.wLength < 2 ? s.wLength : 2;
    a.buf[0] = (s.bmRequestType & 0x1f) == 0 ? 0x01 : 0x00;   // device: self-powered
    a.buf[1] = 0;
    return a;
  case kSetInterface:
  case kClearFeature:
  case kSetFeature:
    a.kind = kStatusOnly;
    return a;
  case kGetInterface:
    a.kind = kTxData;
    a.len = s.wLength ? 1 : 0;
    a.buf[0] = 0;
    return a;
  default:
    a.kind = deviceToHost ? kStall : kStall;
    return a;
  }
}

}  // namespace acmep0

#endif
