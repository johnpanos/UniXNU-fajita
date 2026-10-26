// SPDX-License-Identifier: BSD-2-Clause-Patent
// fjdbg: the framed, multiplexed byte protocol of the fajita USB debug channel.
//
// One byte stream (the CDC-ACM bulk pair NativeDwc3 will expose as /dev/ttyACM0)
// carries, interleaved:
//   - plain console text (anything outside a frame),
//   - fjdbg frames:  0xFC  escaped(payload || crc32le(payload))  0xFB
//   - XNU serial-KDP frames (osfmk/kdp/kdp_serial.c, start byte 0xFA), so the same
//     port can also carry a stock `kdp_match_name=serial` debugger session.
// Escaping (both frame kinds): 0xFA 0xFB 0xFC 0xFE and '\n' are sent as 0xFE, ~byte,
// so a frame never contains a start/end byte or a newline that a console path could
// rewrite to CR/LF. The CRC is the reflected CRC-32 (zlib's crc32, polynomial
// 0xEDB88320) of the unescaped payload.
//
// Payload: u8 channel, u8 flags (0), u16 seq (LE), data. Channels:
//   CTRL 0  reserved for link management (hello/keepalive); unused in v1
//   LOG  1  device -> host: u64 stream offset of the first byte, then kernel log text
//   CMD  2  host -> device: a request (UsbDbgDevice.h); the reply echoes seq
//   RSP  3  device -> host: the reply
//   KDP  4  both ways: one Ethernet frame of the KDP UDP link (kdp_register_send_receive)
//   SHELL 5 both ways: raw bytes to/from the userland agent behind /dev/fjdbg
//   FILE 6  device -> host: a bulk object (a screenshot): u32 id, u32 byte offset, then bytes
//   NET  7  both ways: one Ethernet frame (no FCS, at most kMaxData bytes) per frame; usb-net=1, see UsbNet.h
//
// The host side is scripts/fajita-usbdbg.py, which mirrors these constants; the host
// tests (tests/UsbDbgProto.cpp, tests/run.sh) check that both agree byte for byte.
//
// Freestanding: only <stdint.h>/<stddef.h>, no allocation, no locks, no libc. It is
// compiled into the kernel (the polled gadget path must work from the panic and
// debugger contexts) and on the host by the tests.
#ifndef NATIVE_DWC3_USBDBG_PROTO_H
#define NATIVE_DWC3_USBDBG_PROTO_H

#include <stddef.h>
#include <stdint.h>

namespace usbdbg {

enum : uint8_t {
  kFrameStart = 0xFC,
  kFrameEnd = 0xFB,
  kFrameEsc = 0xFE,
  kKdpStart = 0xFA,   // osfmk/kdp/kdp_serial.c SKDP_START_CHAR (its END and ESC are ours too)
  kProtoVersion = 1,
};

enum Channel : uint8_t { kChCtrl = 0, kChLog = 1, kChCmd = 2, kChRsp = 3, kChKdp = 4, kChShell = 5, kChFile = 6, kChNet = 7 };

enum : size_t {
  kHeaderBytes = 4,
  kMaxData = 1536,   // a full 1518-byte Ethernet frame on the KDP channel fits
  kMaxPayload = kHeaderBytes + kMaxData,
  kCrcBytes = 4,
  // Worst case on the wire: start + every payload/CRC byte escaped + end.
  kMaxEncoded = 2 + 2 * (kMaxPayload + kCrcBytes),
};

static inline uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len)
{
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

static inline bool needsEscape(uint8_t b)
{
  return b == kKdpStart || b == kFrameEnd || b == kFrameStart || b == kFrameEsc || b == '\n';
}

static inline void putLe16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
static inline void putLe32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }
static inline void putLe64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (8 * i)); }
static inline uint16_t getLe16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
static inline uint32_t getLe32(const uint8_t *p)
{
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
static inline uint64_t getLe64(const uint8_t *p) { return uint64_t(getLe32(p)) | (uint64_t(getLe32(p + 4)) << 32); }

// Escaping writer into a bounded buffer; `ok` drops to false on overflow.
struct Writer {
  uint8_t *out;
  size_t cap, at;
  bool ok;
  void raw(uint8_t b)
  {
    if (at < cap) out[at++] = b; else ok = false;
  }
  void esc(uint8_t b)
  {
    if (needsEscape(b)) { raw(kFrameEsc); raw(uint8_t(~b)); } else { raw(b); }
  }
};

// Encode one frame: header + `len` data bytes. Returns the encoded length, or 0 if
// `len` exceeds kMaxData or `cap` is too small (nothing useful is left in `out`).
static inline size_t encodeFrame(uint8_t channel, uint16_t seq, const uint8_t *data, size_t len, uint8_t *out, size_t cap)
{
  if (len > kMaxData) return 0;
  uint8_t header[kHeaderBytes] = {channel, 0, uint8_t(seq), uint8_t(seq >> 8)};
  uint32_t crc = crc32Update(0, header, sizeof header);
  crc = crc32Update(crc, data, len);
  Writer w{out, cap, 0, true};
  w.raw(kFrameStart);
  for (size_t i = 0; i < sizeof header; i++) w.esc(header[i]);
  for (size_t i = 0; i < len; i++) w.esc(data[i]);
  for (int i = 0; i < 4; i++) w.esc(uint8_t(crc >> (8 * i)));
  w.raw(kFrameEnd);
  return w.ok ? w.at : 0;
}

// Incremental decoder of fjdbg frames. feed() takes one byte at a time:
//   kText   the byte is outside any frame (console text; the device side ignores it)
//   kBusy   the byte was consumed by a frame in progress
//   kFrame  a complete frame with a good CRC is ready: channel()/seq()/data()/length()
//   kBad    a frame was dropped (bad CRC, too long, too short, bad escape); counted
// Serial-KDP frames (0xFA) are not decoded here: the device never receives them on
// this channel, and the host tool handles them itself.
struct Decoder {
  enum Result { kText, kBusy, kFrame, kBad };
  enum State : uint8_t { kIdle, kIn, kInEsc };
  State state = kIdle;
  size_t pos = 0;
  uint32_t frames = 0, bad = 0;
  uint8_t buf[kMaxPayload + kCrcBytes] = {};   // every member initialized: constant-initialized, no guard variable

  void reset() { state = kIdle; pos = 0; }
  uint8_t channel() const { return buf[0]; }
  uint16_t seq() const { return getLe16(buf + 2); }
  const uint8_t *data() const { return buf + kHeaderBytes; }
  size_t length() const { return pos - kHeaderBytes - kCrcBytes; }

  Result feed(uint8_t b)
  {
    switch (state) {
    case kIdle:
      if (b == kFrameStart) { state = kIn; pos = 0; return kBusy; }
      return kText;
    case kInEsc:
      if (b == kFrameStart || b == kFrameEnd) { return drop(b); }   // never valid after ESC
      state = kIn;
      return put(uint8_t(~b));
    case kIn:
      if (b == kFrameEsc) { state = kInEsc; return kBusy; }
      if (b == kFrameStart) { bad++; pos = 0; return kBusy; }        // restart: the old frame was cut
      if (b == kFrameEnd) { state = kIdle; return finish(); }
      return put(b);
    }
    return kText;
  }

private:
  Result put(uint8_t b)
  {
    if (pos == sizeof buf) { state = kIdle; pos = 0; bad++; return kBad; }
    buf[pos++] = b;
    return kBusy;
  }
  Result drop(uint8_t b)
  {
    bad++;
    pos = 0;
    state = (b == kFrameStart) ? kIn : kIdle;
    return kBad;
  }
  Result finish()
  {
    if (pos < kHeaderBytes + kCrcBytes) { bad++; return kBad; }
    size_t body = pos - kCrcBytes;
    if (crc32Update(0, buf, body) != getLe32(buf + body) || buf[1] != 0) { bad++; return kBad; }
    frames++;
    return kFrame;
  }
};

}  // namespace usbdbg

#endif
