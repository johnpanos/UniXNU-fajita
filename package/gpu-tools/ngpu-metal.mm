// SPDX-License-Identifier: MIT
// ngpu-metal: Metal device discovery and a first GPU command from an ordinary process (run from the usb-debug agent, outside every
// sandbox). With the NativeAdrenoMetal plugin installed and this process listed in NativeAdrenoMetal.clients, Metal's own discovery
// (IOAcceleratorES -> MetalPluginName -> the plugin) returns the native device: metal2vulkan on Turnip on the Adreno 630.
//   ngpu-metal [fill]   discovery, then a shared buffer filled by a blit command buffer and read back
// One line per step; the exit status is the first failed step (0 = all passed).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

static double nowMs() {
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    return (double)mach_absolute_time() * base.numer / base.denom / 1e6;
}

int main(int argc, char **argv) {
    // A hung GPU or Metal call must not wedge the usb-debug agent (it runs one command at a time): exit after NGPU_LIMIT s (20).
    signal(SIGALRM, [](int) {static const char m[] = "FAIL timeout (alarm)\n"; write(1, m, sizeof m - 1); _exit(124);});
    alarm(getenv("NGPU_LIMIT") ? (unsigned)atoi(getenv("NGPU_LIMIT")) : 20);
    @autoreleasepool {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        const double t0 = nowMs();
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();   // MTLCopyAllDevices is iOS 18+
        printf("ok 1 discovery returned in %.0f ms\n", nowMs() - t0);
        if (!device) {printf("FAIL 2 MTLCreateSystemDefaultDevice: nil\n"); return 2;}
        printf("ok 2 device '%s' class %s registryID 0x%llx apple3 %d common2 %d maxBuffer %llu\n", device.name.UTF8String,
               object_getClassName(device), (unsigned long long)device.registryID, [device supportsFamily:MTLGPUFamilyApple3],
               [device supportsFamily:MTLGPUFamilyCommon2], (unsigned long long)device.maxBufferLength);
        if (argc > 1 && strcmp(argv[1], "fill")) return 0;
        const NSUInteger bytes = 1u << 16;
        id<MTLBuffer> buffer = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (!buffer) {printf("FAIL 3 newBufferWithLength\n"); return 3;}
        memset(buffer.contents, 0x5a, bytes);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {printf("FAIL 4 newCommandQueue\n"); return 4;}
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (!commands || !blit) {printf("FAIL 5 command buffer / blit encoder\n"); return 5;}
        [blit fillBuffer:buffer range:NSMakeRange(0, bytes / 2) value:0xa6];
        [blit endEncoding];
        const double t1 = nowMs();
        [commands commit];
        [commands waitUntilCompleted];
        const double t2 = nowMs();
        printf("ok 6 command buffer status %ld error '%s' in %.1f ms\n", (long)commands.status,
               commands.error ? commands.error.localizedDescription.UTF8String : "", t2 - t1);
        const uint8_t *p = (const uint8_t *)buffer.contents;
        NSUInteger filled = 0, untouched = 0;
        for (NSUInteger i = 0; i < bytes; ++i) {
            if (i < bytes / 2 && p[i] == 0xa6) ++filled;
            if (i >= bytes / 2 && p[i] == 0x5a) ++untouched;
        }
        const bool ok = filled == bytes / 2 && untouched == bytes / 2;
        printf("%s 7 GPU fill: %lu/%lu filled, %lu/%lu untouched\n", ok ? "ok" : "FAIL", (unsigned long)filled, (unsigned long)(bytes / 2),
               (unsigned long)untouched, (unsigned long)(bytes / 2));
        if (!ok) return 7;
        printf("PASS ngpu-metal\n");
    }
    return 0;
}
