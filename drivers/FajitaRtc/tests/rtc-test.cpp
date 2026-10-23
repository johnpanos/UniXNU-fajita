// Host test of RtcCore.h: the counter read with its carry check against a PM8998 RTC model, the offset record and the wall time.
#include "../RtcCore.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// A model PMIC: 64 KiB of registers; the RTC counter advances by `tick_after` after a given number of byte reads.
struct Model {
    uint8_t reg[0x10000] = {};
    unsigned reads = 0, tick_at = ~0u;
    int fail = 0;
    void setCounter(uint32_t v) { for (int i = 0; i < 4; ++i) reg[0x6048 + i] = uint8_t(v >> (8 * i)); }
    uint32_t counter() const { uint32_t v = 0; for (int i = 3; i >= 0; --i) v = v << 8 | reg[0x6048 + i]; return v; }
};

static int modelRead(void *ctx, unsigned addr, uint8_t *buf, unsigned len) {
    Model *m = static_cast<Model *>(ctx);
    if (m->fail) return m->fail;
    if ((addr & 0xff00) != ((addr + len - 1) & 0xff00)) return -22;
    for (unsigned i = 0; i < len; ++i) {
        if (m->reads++ == m->tick_at) m->setCounter(m->counter() + 1);
        buf[i] = m->reg[addr + i];
    }
    return 0;
}

int main() {
    // Plain read: the value measured on the phone.
    {
        Model m; m.setCounter(0x0001ae31);
        uint32_t s = 0;
        CHECK(rtc_read_counter(modelRead, &m, RTC_PM8941_RW_BASE, &s) == 0 && s == 0x0001ae31);
    }
    // Carry through the read: 0x000001ff -> 0x00000200 after the low byte was read. The torn value would be 0x000002ff.
    {
        Model m; m.setCounter(0x1ff); m.tick_at = 1;
        uint32_t s = 0;
        CHECK(rtc_read_counter(modelRead, &m, RTC_PM8941_RW_BASE, &s) == 0 && s == 0x200);
    }
    // A tick between the bulk read and the low byte check without a carry keeps the first value (one second old at most).
    {
        Model m; m.setCounter(0x1fe); m.tick_at = 4;
        uint32_t s = 0;
        CHECK(rtc_read_counter(modelRead, &m, RTC_PM8941_RW_BASE, &s) == 0 && s == 0x1fe);
    }
    // Errors pass through.
    {
        Model m; m.fail = -60;
        uint32_t s = 7;
        CHECK(rtc_read_counter(modelRead, &m, RTC_PM8941_RW_BASE, &s) == -60 && s == 7);
    }
    // Record round trip, corruption, blank and version.
    {
        std::vector<uint8_t> b(RTC_RECORD_BLOCK);
        rtc_record r = {5, 1792686397, 1792796530, 0x1ae37}, o = {};
        rtc_record_encode(&r, b.data());
        CHECK(rtc_record_decode(b.data(), &o) && o.seq == 5 && o.offset == 1792686397 && o.wall == 1792796530 && o.raw == 0x1ae37);
        for (size_t i = RTC_RECORD_BYTES; i < b.size(); ++i) CHECK(b[i] == 0);
        b[30] ^= 1; CHECK(!rtc_record_decode(b.data(), &o)); b[30] ^= 1;
        b[8] = 2; CHECK(!rtc_record_decode(b.data(), &o)); b[8] = 1;
        CHECK(rtc_record_decode(b.data(), &o));
        std::vector<uint8_t> z(RTC_RECORD_BLOCK, 0);
        CHECK(!rtc_record_decode(z.data(), &o));
        std::vector<uint8_t> f(RTC_RECORD_BLOCK, 0xff);
        CHECK(!rtc_record_decode(f.data(), &o));
        // Negative offsets survive the encoding.
        rtc_record n = {1, -5, 10, 15};
        rtc_record_encode(&n, b.data());
        CHECK(rtc_record_decode(b.data(), &o) && o.offset == -5);
    }
    // CRC: the zlib check value.
    CHECK(rtc_crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xcbf43926u);
    // Pick.
    {
        rtc_record a = {3, 0, 0, 0}, b = {4, 0, 0, 0};
        CHECK(rtc_record_pick(1, &a, 1, &b) == 1);
        CHECK(rtc_record_pick(1, &b, 1, &a) == 0);
        CHECK(rtc_record_pick(1, &a, 0, &b) == 0);
        CHECK(rtc_record_pick(0, &a, 1, &b) == 1);
        CHECK(rtc_record_pick(0, &a, 0, &b) == -1);
    }
    // Wall time.
    {
        rtc_record r = {1, 1792686397, 1792796530, 110133};
        uint64_t t = 0;
        CHECK(rtc_wall_time(110200, 0, &r, &t) == RTC_TIME_NONE);
        CHECK(rtc_wall_time(110200, 1, &r, &t) == RTC_TIME_OK && t == 1792686397ull + 110200);
        CHECK(rtc_wall_time(110133, 1, &r, &t) == RTC_TIME_OK && t == 1792796530ull);
        CHECK(rtc_wall_time(40, 1, &r, &t) == RTC_TIME_RESET && t == 1792796530ull + 40);   // the RTC restarted from 0
        rtc_record neg = {1, -1000, 0, 0};
        CHECK(rtc_wall_time(10, 1, &neg, &t) == RTC_TIME_NONE);
    }
    // Set: first record, drift below the slack, a real change, an RTC reset; slot alternation by seq.
    {
        rtc_record prev = {}, next = {};
        CHECK(rtc_record_set(110133, 1792796530, 0, &prev, 2, &next) == 1 && next.seq == 1 && next.offset == 1792686397);
        prev = next;
        CHECK(rtc_record_set(120000, 1792686397ull + 120001, 1, &prev, 2, &next) == 0 && next.seq == 2 && next.offset == 1792686398);
        CHECK(rtc_record_set(120000, 1792686397ull + 120002, 1, &prev, 2, &next) == 1);
        CHECK(rtc_record_set(120000, 1792686397ull + 119990, 1, &prev, 2, &next) == 1 && next.offset == 1792686387);
        CHECK(rtc_record_set(5, 1792900000, 1, &prev, 2, &next) == 1 && next.raw == 5);
        uint64_t t = 0;
        CHECK(rtc_wall_time(6, 1, &next, &t) == RTC_TIME_OK && t == 1792900001);
    }
    if (failures) { std::printf("rtc-test: %d failures\n", failures); return 1; }
    std::printf("rtc-test: pass\n");
    return 0;
}
