// SPDX-License-Identifier: MIT
// ngpu-probe: the first userland test of fajita's A630 kernel service. It runs from the usb-debug agent (`run /usr/sbin/ngpu-probe`),
// outside every sandbox, and drives NativeAdreno's user-client ABI (drivers/NativeAdreno/NativeAdrenoABI.h) directly: open the published
// "NativeAdrenoGpu" service (open type 0, selector = ABI method), Query, GetDeviceInfo, BO alloc/map/CPU-map, an indirect buffer with a
// CP_MEM_WRITE written from userspace, Submit, WaitFence, and a read-back of the value the CP wrote. Then two timestamps and the status
// counters. One line per step on stdout; the exit status is the number of the first failed step (0 = all passed).
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <initializer_list>
#include "NativeAdrenoABI.h"

static io_connect_t gConn;
static kern_return_t call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize) {
    size_t got = outSize;
    return IOConnectCallStructMethod(gConn, method, in, inSize, outSize ? out : nullptr, outSize ? &got : nullptr);
}
// PM4 TYPE7 header with the odd-parity bits (Mesa pm4_pkt7_hdr).
static uint32_t parity(uint32_t v) {v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v &= 0xf; return (~0x6996u >> v) & 1;}
static uint32_t pkt7(uint32_t opcode, uint32_t count) {
    return 0x70000000u | count | (parity(count) << 15) | ((opcode & 0x7f) << 16) | (parity(opcode) << 23);
}
static int fail(int step, const char *what, kern_return_t kr) {printf("FAIL %d %s: 0x%x\n", step, what, kr); return step;}

struct Bo {uint64_t handle = 0, iova = 0, bytes = 0; uint32_t token = 0; volatile uint32_t *cpu = nullptr;};
static kern_return_t makeBo(Bo &bo, uint64_t bytes, uint64_t iova) {
    NativeAdrenoAbiBoAlloc alloc{bytes, 12, 0};
    NativeAdrenoAbiBoAllocReply allocReply{};
    kern_return_t kr = call(NativeAdrenoMethodBoAlloc, &alloc, sizeof alloc, &allocReply, sizeof allocReply);
    if (kr) return kr;
    bo.handle = allocReply.handle;
    bo.bytes = bytes;
    NativeAdrenoAbiBoMap map{bo.handle, iova, 3, 0, 2, 0};
    NativeAdrenoAbiBoMapReply mapReply{};
    if ((kr = call(NativeAdrenoMethodBoMap, &map, sizeof map, &mapReply, sizeof mapReply))) return kr;
    bo.iova = mapReply.iova;
    NativeAdrenoAbiBoCpuMap cpu{bo.handle};
    NativeAdrenoAbiBoCpuMapReply cpuReply{};
    if ((kr = call(NativeAdrenoMethodBoCpuMap, &cpu, sizeof cpu, &cpuReply, sizeof cpuReply))) return kr;
    bo.token = cpuReply.token;
    mach_vm_address_t address = 0;
    mach_vm_size_t size = 0;
    if ((kr = IOConnectMapMemory64(gConn, bo.token, mach_task_self(), &address, &size, kIOMapAnywhere | kIOMapWriteCombineCache))) return kr;
    bo.cpu = reinterpret_cast<volatile uint32_t *>(address);
    return KERN_SUCCESS;
}

int main(int argc, char **argv) {
    const char *names[] = {"NativeAdrenoGpuPublished", nullptr};
    io_service_t service = 0;
    for (unsigned i = 0; i < 40 && !service; ++i) {
        service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(names[0]));
        if (!service) service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceNameMatching("NativeAdrenoGpu"));
        if (!service) usleep(250000);
    }
    if (!service) return fail(1, "no NativeAdrenoGpu service", 0);
    kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &gConn);
    IOObjectRelease(service);
    if (kr) return fail(2, "IOServiceOpen type 0", kr);
    printf("ok 2 open\n");

    NativeAdrenoAbiParams params{};
    if ((kr = call(NativeAdrenoMethodQuery, nullptr, 0, &params, sizeof params))) return fail(3, "Query", kr);
    printf("ok 3 query abi=%u input_bits=%u output_bits=%u flags=0x%x max_bo=0x%llx\n", params.abi_version, params.input_bits,
           params.output_bits, params.flags, (unsigned long long)params.max_bo_bytes);
    if (params.abi_version != NativeAdrenoAbiVersion) return fail(3, "abi version", params.abi_version);

    NativeAdrenoAbiDeviceInfo info{};
    if ((kr = call(NativeAdrenoMethodGetDeviceInfo, nullptr, 0, &info, sizeof info))) return fail(4, "GetDeviceInfo", kr);
    printf("ok 4 device chip=0x%08x gmem=0x%x@0x%llx trap=0x%llx va=0x%llx+0x%llx hbb=%u swizzle=%u macrotile=%u ubwc=%u flags=0x%x\n",
           info.chip_id, info.gmem_bytes, (unsigned long long)info.gmem_base, (unsigned long long)info.uche_trap_base,
           (unsigned long long)info.va_start, (unsigned long long)info.va_size, info.highest_bank_bit, info.ubwc_swizzle,
           info.macrotile_mode, info.ubwc_version, info.flags);

    Bo ib, target;
    const uint64_t base = info.va_start;
    if ((kr = makeBo(ib, 0x4000, base))) return fail(5, "IB BO alloc/map/cpu-map", kr);
    if ((kr = makeBo(target, 0x4000, base + 0x10000))) return fail(5, "target BO alloc/map/cpu-map", kr);
    printf("ok 5 bos ib=0x%llx target=0x%llx tokens %u/%u\n", (unsigned long long)ib.iova, (unsigned long long)target.iova, ib.token,
           target.token);

    const uint32_t magic = argc > 1 ? (uint32_t)strtoul(argv[1], nullptr, 0) : 0xa630c0deu;
    for (unsigned i = 0; i < 16; ++i) target.cpu[i] = 0x5a5a5a5au;
    unsigned n = 0;
    ib.cpu[n++] = pkt7(0x3d, 5);                         // CP_MEM_WRITE: address, then three values
    ib.cpu[n++] = (uint32_t)target.iova;
    ib.cpu[n++] = (uint32_t)(target.iova >> 32);
    ib.cpu[n++] = magic;
    ib.cpu[n++] = ~magic;
    ib.cpu[n++] = 0x630u;
    ib.cpu[n++] = pkt7(0x10, 0);                         // CP_NOP
    __sync_synchronize();

    NativeAdrenoAbiSubmit submit{};
    submit.buffer_count = 1;
    submit.buffers[0] = {ib.handle, ib.iova, n * 4ull};
    NativeAdrenoAbiSubmitReply reply{};
    if ((kr = call(NativeAdrenoMethodSubmit, &submit, sizeof submit, &reply, sizeof reply))) return fail(6, "Submit", kr);
    printf("ok 6 submit fence=%llu (%u dwords)\n", (unsigned long long)reply.fence, n);

    NativeAdrenoAbiWaitFence wait{reply.fence, 2000000000ull};
    if ((kr = call(NativeAdrenoMethodWaitFence, &wait, sizeof wait, nullptr, 0))) return fail(7, "WaitFence 2 s", kr);
    printf("ok 7 fence signalled\n");
    __sync_synchronize();
    const uint32_t a = target.cpu[0], b = target.cpu[1], c = target.cpu[2], d = target.cpu[3];
    printf("%s 8 readback %08x %08x %08x (next %08x)\n", a == magic && b == ~magic && c == 0x630u && d == 0x5a5a5a5au ? "ok" : "FAIL", a, b, c, d);
    if (!(a == magic && b == ~magic && c == 0x630u)) return 8;

    NativeAdrenoAbiTimestamp t0{}, t1{};
    kr = call(NativeAdrenoMethodGetTimestamp, nullptr, 0, &t0, sizeof t0);
    usleep(100000);
    if (!kr) kr = call(NativeAdrenoMethodGetTimestamp, nullptr, 0, &t1, sizeof t1);
    if (kr) printf("warn 9 GetTimestamp: 0x%x\n", kr);
    else printf("ok 9 timestamp %llu -> %llu (%.2f MHz over 100 ms)\n", (unsigned long long)t0.ticks, (unsigned long long)t1.ticks,
                (double)(t1.ticks - t0.ticks) / 100000.0);
    NativeAdrenoAbiStatus status{};
    if (!call(NativeAdrenoMethodGetStatus, nullptr, 0, &status, sizeof status))
        printf("ok 10 status faults=%llu suspends=%llu\n", (unsigned long long)status.faults, (unsigned long long)status.suspends);

    // Throughput: 200 back-to-back submissions of the same IB, then one wait.
    uint64_t last = 0;
    for (unsigned i = 0; i < 200; ++i) {
        if ((kr = call(NativeAdrenoMethodSubmit, &submit, sizeof submit, &reply, sizeof reply))) return fail(11, "Submit loop", kr);
        last = reply.fence;
        if (i % 6 == 5) {NativeAdrenoAbiWaitFence w{last, 2000000000ull}; if ((kr = call(NativeAdrenoMethodWaitFence, &w, sizeof w, nullptr, 0))) return fail(11, "WaitFence loop", kr);}
    }
    NativeAdrenoAbiWaitFence w{last, 2000000000ull};
    if ((kr = call(NativeAdrenoMethodWaitFence, &w, sizeof w, nullptr, 0))) return fail(11, "WaitFence after loop", kr);
    printf("ok 11 200 submissions, last fence %llu\n", (unsigned long long)last);

    IOConnectUnmapMemory64(gConn, ib.token, mach_task_self(), (mach_vm_address_t)ib.cpu);
    IOConnectUnmapMemory64(gConn, target.token, mach_task_self(), (mach_vm_address_t)target.cpu);
    for (Bo *bo : {&ib, &target}) {
        NativeAdrenoAbiBoCpuUnmap unmap{bo->handle, bo->token, 0};
        call(NativeAdrenoMethodBoCpuUnmap, &unmap, sizeof unmap, nullptr, 0);
        NativeAdrenoAbiBoFree free_{bo->handle};
        if ((kr = call(NativeAdrenoMethodBoFree, &free_, sizeof free_, nullptr, 0))) printf("warn 12 BoFree: 0x%x\n", kr);
    }
    IOServiceClose(gConn);
    printf("PASS ngpu-probe\n");
    return 0;
}
