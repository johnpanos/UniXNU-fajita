// SPDX-License-Identifier: BSD-2-Clause-Patent
// fajita haptics from iOS: how the Taptic waveform iOS plays on the "Actuator" audio device becomes motor commands. Shared by the
// HAL plug-in (package/fajita-audio/FajitaAudio.c: samples -> levels -> what to send), the kernel sink (package/FajitaHaptics.cpp,
// fajita-haptics=3: levels -> PMI8998 LRA start/update/stop) and the host test (package/tests/fajita-haptic-test.cpp).
//
// iOS's haptics are waveforms of one channel (UIKit feedback and keyboard taps are 10-40 ms bursts, vibrations long runs). The
// PMI8998 drives an LRA at its own resonance, so only the envelope matters: each I/O buffer becomes one level, 0..1000 of full scale
// by its peak (0 below FHP_SILENCE). The plug-in sends a level when it starts or stops, when it moves by FHP_STEP, and every
// FHP_RESEND_MS while it lasts. The kernel starts the motor at a drive voltage for the level, never runs it shorter than FHP_MIN_ON_MS
// (shorter is not felt), stops it FHP_KEEPALIVE_MS after the last level (the plug-in or mediaserverd went away) and never runs it
// longer than FHP_MAX_ON_MS in one go.
#ifndef FAJITA_HAPTIC_POLICY_H
#define FAJITA_HAPTIC_POLICY_H
#include <stdint.h>

#define FHP_SILENCE 1024u        // peak below 1/32 of full scale: silence
#define FHP_STEP 100u            // a level change worth sending
#define FHP_RESEND_MS 50u
#define FHP_MIN_ON_MS 25u
#define FHP_KEEPALIVE_MS 150u
#define FHP_MAX_ON_MS 5000u
#define FHP_VMAX_LOW_MV 1200u    // the drive of the softest level
#define FHP_VMAX_HIGH_MV 3596u   // the PMI8998's highest VMAX step (31 x 116 mV)

// The level of one buffer of 16-bit samples: 0, or FHP_SILENCE..32768 mapped onto 1..1000.
static inline unsigned fhp_level(const int16_t *s, unsigned n) {
    unsigned peak = 0;
    for (unsigned i = 0; i < n; i++) {
        int v = s[i];
        unsigned a = (unsigned)(v < 0 ? -v : v);
        if (a > peak) peak = a;
    }
    if (peak < FHP_SILENCE) return 0;
    unsigned l = (unsigned)(((uint64_t)(peak - FHP_SILENCE) * 999u) / (32768u - FHP_SILENCE)) + 1u;
    return l > 1000u ? 1000u : l;
}

// The motor's drive voltage for a level (1..1000).
static inline unsigned fhp_vmax(unsigned level) {
    if (level > 1000u) level = 1000u;
    return FHP_VMAX_LOW_MV + (unsigned)(((uint64_t)level * (FHP_VMAX_HIGH_MV - FHP_VMAX_LOW_MV)) / 1000u);
}

// The plug-in's side: whether to send `level` now, given what it sent last.
typedef struct {
    unsigned sent;        // the level sent last (0: stopped)
    uint64_t sent_ms;
    uint32_t sends;
} fhp_sender;

static inline int fhp_should_send(fhp_sender *s, unsigned level, uint64_t now_ms) {
    int send;
    if ((level == 0) != (s->sent == 0)) send = 1;
    else if (level == 0) send = 0;
    else if ((level > s->sent ? level - s->sent : s->sent - level) >= FHP_STEP) send = 1;
    else send = now_ms - s->sent_ms >= FHP_RESEND_MS;
    if (send) { s->sent = level; s->sent_ms = now_ms; s->sends++; }
    return send;
}

// The kernel's side.
enum { FHP_NONE = 0, FHP_START, FHP_UPDATE, FHP_STOP };
typedef struct {
    int playing;
    unsigned level;
    unsigned stop_pending;
    uint64_t started_ms, last_ms;
    uint32_t starts, stops, timeouts, capped;
} fhp_motor;

// A level arrived. Returns the action; *vmax is the drive for FHP_START / FHP_UPDATE.
static inline int fhp_command(fhp_motor *m, uint64_t now_ms, unsigned level, unsigned *vmax) {
    m->last_ms = now_ms;
    if (level) {
        m->stop_pending = 0;
        if (!m->playing) {
            m->playing = 1; m->level = level; m->started_ms = now_ms; m->starts++;
            *vmax = fhp_vmax(level);
            return FHP_START;
        }
        if ((level > m->level ? level - m->level : m->level - level) >= FHP_STEP) {
            m->level = level;
            *vmax = fhp_vmax(level);
            return FHP_UPDATE;
        }
        return FHP_NONE;
    }
    if (!m->playing) return FHP_NONE;
    if (now_ms - m->started_ms < FHP_MIN_ON_MS) { m->stop_pending = 1; return FHP_NONE; }
    m->playing = 0; m->level = 0; m->stops++;
    return FHP_STOP;
}

// Time passed (called every few ms while playing). Returns FHP_STOP or FHP_NONE.
static inline int fhp_tick(fhp_motor *m, uint64_t now_ms) {
    if (!m->playing) return FHP_NONE;
    int stop = 0;
    if (m->stop_pending && now_ms - m->started_ms >= FHP_MIN_ON_MS) stop = 1;
    else if (now_ms - m->last_ms > FHP_KEEPALIVE_MS) { stop = 1; m->timeouts++; }
    else if (now_ms - m->started_ms > FHP_MAX_ON_MS) { stop = 1; m->capped++; }
    if (!stop) return FHP_NONE;
    m->playing = 0; m->level = 0; m->stop_pending = 0; m->stops++;
    return FHP_STOP;
}

#endif
