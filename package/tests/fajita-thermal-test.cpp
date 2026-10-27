// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the fajita thermal policy (FajitaThermalPolicy.h): TSENS decoding and the cluster step governor.
#include "FajitaThermalPolicy.h"
#include <cstdio>

namespace {
int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

void testDecode() {
    int32_t t = 0;
    CHECK(!ftp_decode(0x1c2, &t));                       // VALID clear
    CHECK(ftp_decode((1u << 21) | 452, &t) && t == 452); // 45.2 C
    CHECK(ftp_decode((1u << 21) | 0xfec, &t) && t == -20);   // -2.0 C, 12-bit two's complement
    CHECK(ftp_decode((1u << 21) | (0x3ffu << 12) | 300, &t) && t == 300);   // bits above 11 ignored
}

void testSteps() {
    ftp_trips tr = ftp_default_trips();
    ftp_cluster c;
    ftp_init(&c, 3, 15);
    CHECK(c.level == 15);
    CHECK(!ftp_step(&c, &tr, 500, 1) && c.level == 15);   // cool, already at the top
    CHECK(ftp_step(&c, &tr, 900, 1) && c.level == 14);    // trip
    CHECK(ftp_step(&c, &tr, 920, 1) && c.level == 13);    // still above trip: keeps stepping
    CHECK(ftp_step(&c, &tr, 960, 1) && c.level == 11);    // hot: two
    CHECK(!ftp_step(&c, &tr, 870, 1) && c.level == 11);   // between release and trip: holds
    for (unsigned i = 0; i < tr.hold - 1; ++i) CHECK(!ftp_step(&c, &tr, 800, 1));
    CHECK(ftp_step(&c, &tr, 800, 1) && c.level == 12);    // after `hold` quiet seconds: one up
    CHECK(!ftp_step(&c, &tr, 860, 1));                    // warm again: the quiet count restarts
    for (unsigned i = 0; i < tr.hold - 1; ++i) CHECK(!ftp_step(&c, &tr, 800, 1));
    CHECK(ftp_step(&c, &tr, 800, 1) && c.level == 13);
    CHECK(ftp_step(&c, &tr, 1060, 1) && c.level == 3);    // critical: the floor
    CHECK(!ftp_step(&c, &tr, 1100, 1) && c.level == 3);   // never below it
    CHECK(c.downs == 4 && c.ups == 2);
    for (int i = 0; i < 1000; ++i) ftp_step(&c, &tr, 400, 1);
    CHECK(c.level == 15);                                 // never above the top
    CHECK(!ftp_step(&c, &tr, 2000, 0) && c.invalid == 1 && c.level == 15);   // no reading: nothing moves
}

void testBounds() {
    ftp_trips tr = ftp_default_trips();
    ftp_cluster c;
    ftp_init(&c, 5, 2);                                   // top below floor: pinned at the floor
    CHECK(c.level == 5 && c.top == 5);
    CHECK(!ftp_step(&c, &tr, 1000, 1) && c.level == 5);
    ftp_init(&c, -3, 4);
    CHECK(c.floor == 0);
    ftp_step(&c, &tr, 960, 1);
    ftp_step(&c, &tr, 960, 1);
    ftp_step(&c, &tr, 960, 1);
    CHECK(c.level == 0);
}

void testHottest() {
    int32_t deci[32] = {};
    uint8_t ok[32] = {};
    int32_t t = 0;
    CHECK(!ftp_hottest(deci, ok, ftp_silver, 5, &t));
    deci[2] = 610; ok[2] = 1; deci[5] = 640; ok[5] = 1; deci[7] = 990; ok[7] = 1;
    CHECK(ftp_hottest(deci, ok, ftp_silver, 5, &t) && t == 640);   // L3 cluster0 counts for silver
    CHECK(ftp_hottest(deci, ok, ftp_gold, 5, &t) && t == 990);
    deci[3] = -50; ok[3] = 1; ok[2] = ok[5] = 0;
    CHECK(ftp_hottest(deci, ok, ftp_silver, 5, &t) && t == -50);   // a negative reading is still the hottest valid one
}
}  // namespace

int main() {
    testDecode();
    testSteps();
    testBounds();
    testHottest();
    if (failures) {std::printf("%d failure(s)\n", failures); return 1;}
    std::printf("fajita thermal policy: all checks passed\n");
    return 0;
}
