// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the fjdbg framing (UsbDbgProto.h) and command layer (UsbDbgDevice.h).
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../UsbDbgDevice.h"

using namespace usbdbg;

static std::vector<uint8_t> encode(uint8_t ch, uint16_t seq, const std::vector<uint8_t> &d)
{
  std::vector<uint8_t> out(kMaxEncoded);
  size_t n = encodeFrame(ch, seq, d.data(), d.size(), out.data(), out.size());
  assert(n);
  out.resize(n);
  return out;
}

struct Fake {
  uint8_t mem[256];
  uint32_t reg[4] = {0x5533330au, 0, 0, 0};
  int resets = 0;
};

static int32_t readVirt(void *c, uint64_t a, void *o, uint32_t n)
{
  Fake *f = static_cast<Fake *>(c);
  if (a < 0x1000 || a + n > 0x1000 + sizeof f->mem) return kErrFault;
  memcpy(o, f->mem + (a - 0x1000), n);
  return kOk;
}
static int32_t writeVirt(void *c, uint64_t a, const void *i, uint32_t n)
{
  Fake *f = static_cast<Fake *>(c);
  if (a < 0x1000 || a + n > 0x1000 + sizeof f->mem) return kErrFault;
  memcpy(f->mem + (a - 0x1000), i, n);
  return kOk;
}
static int32_t read32(void *c, uint64_t pa, uint32_t *o, uint32_t n)
{
  Fake *f = static_cast<Fake *>(c);
  if (pa < 0xa60c120 || (pa - 0xa60c120) / 4 + n > 4) return kErrFault;
  for (uint32_t i = 0; i < n; i++) o[i] = f->reg[(pa - 0xa60c120) / 4 + i];
  return kOk;
}
static int32_t reset(void *c, uint32_t) { static_cast<Fake *>(c)->resets++; return kOk; }

int main()
{
  // CRC-32 check value of "123456789".
  const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  assert(crc32Update(0, check, sizeof check) == 0xCBF43926u);

  // Round trip with every byte value (all escapes exercised), text around the frame.
  std::vector<uint8_t> all(256);
  for (int i = 0; i < 256; i++) all[i] = uint8_t(i);
  auto wire = encode(kChLog, 0xFA0A, all);
  for (size_t i = 1; i + 1 < wire.size(); i++) {
    assert(wire[i] != kFrameStart && wire[i] != kFrameEnd && wire[i] != kKdpStart && wire[i] != '\n');
  }
  std::vector<uint8_t> stream = {'h', 'i', '\n'};
  stream.insert(stream.end(), wire.begin(), wire.end());
  stream.push_back('x');
  Decoder d;
  std::string text;
  int frames = 0;
  for (uint8_t b : stream) {
    auto r = d.feed(b);
    if (r == Decoder::kText) text.push_back(char(b));
    if (r == Decoder::kFrame) {
      frames++;
      assert(d.channel() == kChLog && d.seq() == 0xFA0A && d.length() == 256);
      assert(memcmp(d.data(), all.data(), 256) == 0);
    }
    assert(r != Decoder::kBad);
  }
  assert(frames == 1 && text == "hi\nx");

  // A corrupted frame is dropped and the next one still decodes; a cut frame resyncs.
  auto good = encode(kChCmd, 7, {1, 2, 3});
  auto bad = good;
  bad[3] ^= 0x40;
  std::vector<uint8_t> s2(bad);
  s2.insert(s2.end(), good.begin(), good.begin() + 4);   // cut short by a new start
  s2.insert(s2.end(), good.begin(), good.end());
  Decoder d2;
  int ok = 0, dropped = 0;
  for (uint8_t b : s2) {
    auto r = d2.feed(b);
    ok += r == Decoder::kFrame;
    dropped += r == Decoder::kBad;
  }
  assert(ok == 1 && dropped == 1 && d2.bad == 2 && d2.seq() == 7);

  // Oversized data is refused by the encoder.
  std::vector<uint8_t> huge(kMaxData + 1), out(kMaxEncoded * 2);
  assert(encodeFrame(kChKdp, 0, huge.data(), huge.size(), out.data(), out.size()) == 0);

  // Command layer.
  Fake f;
  for (int i = 0; i < 256; i++) f.mem[i] = uint8_t(i ^ 0x5a);
  Target t{};
  t.ctx = &f;
  t.ident = "test";
  t.features = kFeatMemory | kFeatMmio;
  t.readVirt = readVirt;
  t.writeVirt = writeVirt;
  t.read32 = read32;
  t.reset = reset;
  alignas(4) uint8_t rsp[kMaxData];
  uint8_t req[64] = {};

  putLe16(req, kOpHello);
  size_t n = handleCommand(t, req, 4, rsp);
  assert(getLe16(rsp) == (kOpHello | kOpReplyBit) && int32_t(getLe32(rsp + 4)) == kOk);
  assert(rsp[8] == kProtoVersion && getLe16(rsp + 10) == kMaxData && n == 8 + 8 + 4);
  assert(getLe32(rsp + 12) == (kFeatMemory | kFeatMmio));

  putLe16(req, kOpReadVirt);
  putLe64(req + 4, 0x1010);
  putLe32(req + 12, 16);
  n = handleCommand(t, req, 16, rsp);
  assert(n == 8 + 16 && rsp[8] == (0x10 ^ 0x5a));
  putLe64(req + 4, 0x2000);   // unmapped
  n = handleCommand(t, req, 16, rsp);
  assert(n == 8 && int32_t(getLe32(rsp + 4)) == kErrFault);
  putLe32(req + 12, kMaxData);   // too big for one reply
  n = handleCommand(t, req, 16, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kErrTooBig);
  n = handleCommand(t, req, 10, rsp);   // short args
  assert(int32_t(getLe32(rsp + 4)) == kErrArgs);

  putLe16(req, kOpRead32);
  putLe64(req + 4, 0xa60c120);
  putLe32(req + 12, 1);
  n = handleCommand(t, req, 16, rsp);
  assert(n == 12 && getLe32(rsp + 8) == 0x5533330au);
  putLe64(req + 4, 0xa60c122);   // unaligned
  assert(int32_t(getLe32(rsp + 4)) == kOk);
  handleCommand(t, req, 16, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kErrArgs);

  // Writes are refused on a read-only session, accepted with allowWrites.
  putLe16(req, kOpWriteVirt);
  putLe64(req + 4, 0x1000);
  req[12] = 0xee;
  handleCommand(t, req, 13, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kErrDenied && f.mem[0] == 0x5a);
  putLe16(req, kOpReset);
  putLe32(req + 4, 0);
  handleCommand(t, req, 8, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kErrDenied && f.resets == 0);
  t.allowWrites = true;
  putLe16(req, kOpWriteVirt);
  putLe64(req + 4, 0x1000);
  req[12] = 0xee;
  handleCommand(t, req, 13, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kOk && f.mem[0] == 0xee);
  putLe16(req, kOpReset);
  putLe32(req + 4, 0);
  handleCommand(t, req, 8, rsp);
  assert(f.resets == 1);

  // Unknown op, missing hook.
  putLe16(req, 0x7777);
  handleCommand(t, req, 4, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kErrOp);
  putLe16(req, kOpState);
  handleCommand(t, req, 4, rsp);
  assert(int32_t(getLe32(rsp + 4)) == kErrUnsupported);

  printf("UsbDbgProto: framing and command assertions passed\n");
  return 0;
}
