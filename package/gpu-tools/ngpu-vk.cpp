// SPDX-License-Identifier: MIT
// ngpu-vk: the plugin's own Turnip sequence (NativeMetal/NativeTurnipSmoke.h: instance, device, a fill, a compute pipeline, a render
// pass) on the A630, without Metal or metal2vulkan, from an ordinary process. Every progress marker is printed as it happens (stdout
// unbuffered), so a run that resets the phone still shows how far it got in the streamed output.
//   ngpu-vk [LIBRARY]   default: the Turnip inside the installed plugin bundle
#include "NativeTurnipSmoke.h"
#include <signal.h>

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    signal(SIGALRM, [](int) {static const char m[] = "FAIL timeout (alarm)\n"; write(1, m, sizeof m - 1); _exit(124);});
    alarm(getenv("NGPU_LIMIT") ? (unsigned)atoi(getenv("NGPU_LIMIT")) : 20);
    const char *library = argc > 1 ? argv[1] : "/System/Library/Extensions/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib";
    auto marker = [&](uint32_t value) { printf("mark %x\n", value); };
    const turnip::Result r = turnip::run(marker, library);
    printf("stage %d result %d fill %d compute %d (step %d) render %d/%d (step %d) ok %d msg '%s'\n", r.stage, r.vkResult, r.verified,
           r.computeWords, r.computeStage, r.renderTriangle, r.renderOther, r.renderStage, r.ok, turnip::gFirstError);
    printf("%s ngpu-vk\n", r.ok ? "PASS" : "FAIL");
    return r.ok ? 0 : 1;
}
