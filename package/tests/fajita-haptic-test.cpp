// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of the iOS haptics -> PMI8998 motor mapping (FajitaHapticPolicy.h): levels from waveforms, what the plug-in sends,
// and the kernel's motor state machine, fed with waveform shapes like iOS's (a 20 ms tap, a 400 ms vibration, silence).
#include "FajitaHapticPolicy.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

std::vector<int16_t> sine(unsigned n, double amp) {
    std::vector<int16_t> v(n);
    for (unsigned i = 0; i < n; ++i) v[i] = int16_t(std::lround(amp * 32767.0 * std::sin(2 * M_PI * 170.0 * i / 48000.0)));
    return v;
}

void testLevel() {
    std::vector<int16_t> z(512, 0);
    CHECK(fhp_level(z.data(), 512) == 0);
    auto quiet = sine(512, 0.02);                    // below 1/32 of full scale
    CHECK(fhp_level(quiet.data(), 512) == 0);
    auto full = sine(512, 1.0);
    CHECK(fhp_level(full.data(), 512) >= 999);
    auto half = sine(512, 0.5);
    unsigned h = fhp_level(half.data(), 512);
    CHECK(h > 450 && h < 520);
    int16_t extreme[2] = {-32768, 0};
    CHECK(fhp_level(extreme, 2) == 1000);            // -32768 has no positive twin: still capped at 1000
    CHECK(fhp_level(nullptr, 0) == 0);
    CHECK(fhp_vmax(1) == FHP_VMAX_LOW_MV + 2);
    CHECK(fhp_vmax(1000) == FHP_VMAX_HIGH_MV);
    CHECK(fhp_vmax(5000) == FHP_VMAX_HIGH_MV);
}

void testSender() {
    fhp_sender s = {};
    CHECK(!fhp_should_send(&s, 0, 0));               // silence from the start: nothing
    CHECK(fhp_should_send(&s, 600, 10));             // starts
    CHECK(!fhp_should_send(&s, 620, 20));            // small change, soon: no
    CHECK(fhp_should_send(&s, 750, 30));             // a step
    CHECK(!fhp_should_send(&s, 760, 70));
    CHECK(fhp_should_send(&s, 760, 80));             // the resend while it lasts
    CHECK(fhp_should_send(&s, 0, 90));               // stops
    CHECK(!fhp_should_send(&s, 0, 500));
    CHECK(s.sends == 4);
}

void testMotor() {
    fhp_motor m = {};
    unsigned v = 0;
    CHECK(fhp_command(&m, 0, 0, &v) == FHP_NONE);
    CHECK(fhp_command(&m, 100, 500, &v) == FHP_START && v == fhp_vmax(500));
    CHECK(fhp_command(&m, 110, 550, &v) == FHP_NONE);
    CHECK(fhp_command(&m, 115, 900, &v) == FHP_UPDATE && v == fhp_vmax(900));
    CHECK(fhp_command(&m, 120, 0, &v) == FHP_NONE && m.stop_pending);    // 20 ms in: held to the minimum
    CHECK(fhp_tick(&m, 124) == FHP_NONE);
    CHECK(fhp_tick(&m, 125) == FHP_STOP && !m.playing);
    CHECK(fhp_command(&m, 200, 400, &v) == FHP_START);
    CHECK(fhp_command(&m, 260, 0, &v) == FHP_STOP);                       // past the minimum: at once
    CHECK(fhp_command(&m, 300, 400, &v) == FHP_START);
    CHECK(fhp_tick(&m, 450) == FHP_NONE);
    CHECK(fhp_tick(&m, 451) == FHP_STOP && m.timeouts == 1);              // the sender went away
    CHECK(fhp_command(&m, 1000, 400, &v) == FHP_START);
    for (uint64_t t = 1040; t <= 1000 + FHP_MAX_ON_MS; t += 40) { fhp_command(&m, t, 400, &v); CHECK(fhp_tick(&m, t) == FHP_NONE); }
    CHECK(fhp_command(&m, 1000 + FHP_MAX_ON_MS + 10, 400, &v) == FHP_NONE);
    CHECK(fhp_tick(&m, 1000 + FHP_MAX_ON_MS + 11) == FHP_STOP && m.capped == 1);
    CHECK(m.starts == m.stops);
}

// The whole chain on a waveform: 10 ms buffers through the sender into the motor, ticked every 5 ms.
struct Run {unsigned starts, stops; uint64_t onMs;};
Run chain(const std::vector<int16_t> &wave) {
    fhp_sender s = {}; fhp_motor m = {};
    Run r = {0, 0, 0};
    uint64_t on = 0;
    const unsigned buf = 480;
    for (uint64_t t = 0, i = 0; t < 2000; t += 5) {
        if (t % 10 == 0) {
            unsigned n = i < wave.size() ? unsigned(std::min<size_t>(buf, wave.size() - i)) : 0;
            unsigned level = n ? fhp_level(wave.data() + i, n) : 0;
            i += buf;
            unsigned v;
            if (fhp_should_send(&s, level, t)) {
                int a = fhp_command(&m, t, level, &v);
                if (a == FHP_START) {++r.starts; on = t;}
                if (a == FHP_STOP) {++r.stops; r.onMs += t - on;}
            }
        }
        if (fhp_tick(&m, t) == FHP_STOP) {++r.stops; r.onMs += t - on;}
    }
    return r;
}

void testChain() {
    std::vector<int16_t> tap = sine(960, 0.8);                           // a 20 ms keyboard tap
    tap.resize(48000, 0);
    Run t = chain(tap);
    CHECK(t.starts == 1 && t.stops == 1 && t.onMs >= FHP_MIN_ON_MS && t.onMs <= 40);
    std::vector<int16_t> buzz = sine(19200, 1.0);                        // a 400 ms vibration
    buzz.resize(48000, 0);
    Run b = chain(buzz);
    CHECK(b.starts == 1 && b.stops == 1 && b.onMs >= 395 && b.onMs <= 420);
    Run q = chain(std::vector<int16_t>(48000, 0));
    CHECK(q.starts == 0 && q.stops == 0);
}
}  // namespace

int main() {
    testLevel();
    testSender();
    testMotor();
    testChain();
    if (failures) {std::printf("%d failure(s)\n", failures); return 1;}
    std::printf("fajita haptic policy: all checks passed\n");
    return 0;
}
