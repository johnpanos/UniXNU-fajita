/* SPDX-License-Identifier: MIT */
/* Runs the plugin's own Turnip smoke sequence (drivers/NativeMetal/NativeTurnipSmoke.h, the code that ships in the Metal plugin) against
 * the host copy of the kernel stack. The software CP executes nothing, so the fill pattern never appears: the expected outcome is that every
 * stage up to and including the fence wait passes and the run stops at stage 10 (only the untouched half of the buffer, 512 words, matches). */
#include "NativeTurnipSmoke.h"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    unsigned marks[32], count = 0, samples = 0;
    auto marker = [&](uint32_t value) { if (count < 32) marks[count++] = value; };
    // The shipping path: the sequence on a worker thread under the watchdog (HOSTSIM_STALL makes the shim block the first BoAlloc).
    const turnip::Result r = turnip::runWatched(marker, [&](const char *text) { printf("watchdog: %s\n", text); ++samples; }, argv[1]);
    printf("watchdog samples: %u\n", samples);
    printf("markers:");
    for (unsigned i = 0; i < count; ++i) printf(" %x", marks[i]);
    printf("\nstage %d result %d fill %d compute %d (step %d) render %d/%d (step %d) ok %d msg '%s'\n", r.stage, r.vkResult, r.verified,
           r.computeWords, r.computeStage, r.renderTriangle, r.renderOther, r.renderStage, r.ok, turnip::gFirstError);
    // The software CP executes nothing: every submission retires, nothing is written. Expected: no setup failure, the fill check sees only the
    // untouched half (512), the compute check sees no shader writes (0), and the render read-back keeps the 0x5a pattern (4096 stray pixels).
    return r.stage == 0 && r.verified == 512 && r.computeWords == 0 && r.computeStage == 0 && r.renderStage == 0 && r.renderTriangle == 0 &&
           r.renderOther == 4096 ? 0 : 1;
}
