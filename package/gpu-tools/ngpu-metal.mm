// SPDX-License-Identifier: MIT
// ngpu-metal: Metal device discovery and a first GPU command from an ordinary process (run from the usb-debug agent, outside every
// sandbox). With the NativeAdrenoMetal plugin installed and this process listed in NativeAdrenoMetal.clients, Metal's own discovery
// (IOAcceleratorES -> MetalPluginName -> the plugin) returns the native device: metal2vulkan on Turnip on the Adreno 630.
//   ngpu-metal [fill]   discovery, then a shared buffer filled by a blit command buffer and read back
//   ngpu-metal cpu SECONDS [NAME...]   per-process CPU time over SECONDS, per-core load, and the threads of each NAME with how often
//                                      each ran or waited (sampled every NGPU_CPU_SAMPLE_MS, default 20); the window's start in mach us
//   ngpu-metal spin THREADS SECONDS [QOS]   per-thread rate of a dependent integer chain per 100 ms slice (core types, placement)
// One line per step; the exit status is the first failed step (0 = all passed).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <algorithm>
#include <pthread.h>
#include <vector>
#include <initializer_list>
#include <signal.h>
// libproc (no header in the iPhoneOS SDK; the functions are in libSystem)
extern "C" int proc_listallpids(void *buffer, int buffersize);
extern "C" int proc_name(int pid, void *buffer, uint32_t buffersize);
extern "C" int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
// sys/proc_info.h's (not in the iPhoneOS SDK): PROC_PIDTASKINFO (task times in mach absolute units), PROC_PIDLISTTHREADS and
// PROC_PIDTHREADINFO (thread times in ns).
struct ProcTaskInfo {
    uint64_t virtualSize, residentSize, totalUser, totalSystem, threadsUser, threadsSystem;
    int32_t policy, faults, pageins, cowFaults, messagesSent, messagesReceived, syscallsMach, syscallsUnix, csw, threadnum, numrunning,
        priority;
};
struct ProcThreadInfo {
    uint64_t userTime, systemTime;
    int32_t cpuUsage, policy, runState, flags, sleepTime, curpri, priority, maxpriority;
    char name[64];
};
enum { kProcTaskInfo = 4, kProcThreadInfo = 5, kProcListThreads = 6 };
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/processor_info.h>
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
    alarm(getenv("NGPU_LIMIT") ? (unsigned)atoi(getenv("NGPU_LIMIT")) : 150);   // device creation compiles the runtime's own pipelines
    @autoreleasepool {
        setvbuf(stdout, nullptr, _IOLBF, 0);
        const double t0 = nowMs();
        if (argc > 2 && !strcmp(argv[1], "killall")) {
            // killall NAME: SIGKILL every process of that name (launchd restarts a KeepAlive job such as backboardd).
            static int pids[4096];
            const int n = proc_listallpids(pids, sizeof pids);
            int killed = 0;
            for (int i = 0; i < n; ++i) {
                char name[64] = {};
                if (proc_name(pids[i], name, sizeof name) > 0 && !strcmp(name, argv[2]) && kill(pids[i], SIGKILL) == 0) {
                    printf("killed %s %d\n", name, pids[i]);
                    ++killed;
                }
            }
            printf("%d killed\n", killed);
            return killed ? 0 : 1;
        }
        if (argc > 1 && !strcmp(argv[1], "constants")) {
            // The private accessors of MTLFunctionConstantValues the native frontend reads: classes and type encodings on this OS.
            MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
            bool b = true; uint32_t u = 7;
            [values setConstantValue:&b type:MTLDataTypeBool withName:@"fc_generic"];
            [values setConstantValue:&u type:MTLDataTypeUInt atIndex:3];
            auto show = [](id object, const char *selector) {
                Method m = class_getInstanceMethod(object_getClass(object), sel_registerName(selector));
                printf("  %s %s: %s\n", class_getName(object_getClass(object)), selector, m ? method_getTypeEncoding(m) : "(none)");
            };
            show(values, "newIndexedConstantArray");
            show(values, "newNamedConstantArray");
            for (const char *sel : {"newIndexedConstantArray", "newNamedConstantArray"}) {
                if (![values respondsToSelector:sel_registerName(sel)]) continue;
                NSArray *array = ((NSArray * (*)(id, SEL))objc_msgSend)(values, sel_registerName(sel));
                printf("%s -> %s count %lu\n", sel, class_getName(object_getClass(array)), (unsigned long)array.count);
                for (id v in array) {show(v, "dataType"); show(v, "data"); show(v, "index"); show(v, "name");}
            }
            return 0;
        }
        if (argc > 2 && !strcmp(argv[1], "ls")) {
            NSArray *names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@(argv[2]) error:nil];
            for (NSString *n in names) printf("%s\n", n.UTF8String);
            return names ? 0 : 1;
        }
        if (argc > 2 && !strcmp(argv[1], "lib")) {
            // lib PATH [FUNCTION]: the native device's view of a metallib (as QuartzCore loads it), and one function from it.
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) {printf("no device\n"); return 2;}
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[2])] error:&error];
            printf("library %s error %s\n", library ? class_getName(object_getClass(library)) : "nil",
                   error ? error.localizedDescription.UTF8String : "-");
            if (!library) return 3;
            NSArray<NSString *> *names = library.functionNames;
            printf("%lu functions:", (unsigned long)names.count);
            for (NSUInteger i = 0; i < names.count && i < 400; ++i) printf(" %s", names[i].UTF8String);
            printf("\n");
            if (argc > 3) {
                MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
                d.name = @(argv[3]);
                MTLFunctionConstantValues *v = [MTLFunctionConstantValues new];
                bool yes = true;
                for (int i = 4; i < argc; ++i) [v setConstantValue:&yes type:MTLDataTypeBool withName:@(argv[i])];
                d.constantValues = v;
                error = nil;
                id<MTLFunction> f = [library newFunctionWithDescriptor:d error:&error];
                printf("function %s: %s error %s\n", argv[3], f ? "made" : "nil", error ? error.localizedDescription.UTF8String : "-");
            }
            return 0;
        }
        if (argc > 3 && !strcmp(argv[1], "spin")) {
            // spin THREADS SECONDS [QOS]: THREADS threads each run a dependent integer chain and report its rate per 100 ms slice (M
            // iterations/s: min, median, max). On a big.LITTLE SoC the kernel schedules without knowing the core types, a thread's slices
            // show which cores it ran on. QOS: ui (USER_INTERACTIVE, the default), default, utility, bg.
            const int n = atoi(argv[2]);
            const double seconds = atof(argv[3]);
            const char *q = argc > 4 ? argv[4] : "ui";
            const qos_class_t qos = !strcmp(q, "bg") ? QOS_CLASS_BACKGROUND : !strcmp(q, "utility") ? QOS_CLASS_UTILITY
                                  : !strcmp(q, "default") ? QOS_CLASS_DEFAULT : QOS_CLASS_USER_INTERACTIVE;
            struct Run {double seconds; qos_class_t qos; std::vector<double> rates; uint64_t sink;};
            std::vector<Run> runs(n > 0 && n <= 64 ? n : 1, Run{seconds, qos, {}, 0});
            std::vector<pthread_t> threads(runs.size());
            for (size_t i = 0; i < runs.size(); ++i) {
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_set_qos_class_np(&attr, qos, 0);
                pthread_create(&threads[i], &attr, [](void *arg) -> void * {
                    Run &r = *(Run *)arg;
                    uint64_t x = 88172645463325252ull;
                    const double end = nowMs() + r.seconds * 1000;
                    while (nowMs() < end) {
                        const double t = nowMs();
                        uint64_t k = 0;
                        while (nowMs() - t < 100) {
                            for (int j = 0; j < 4096; ++j) {x ^= x << 13; x ^= x >> 7; x ^= x << 17;}
                            k += 4096;
                        }
                        r.rates.push_back(k / (nowMs() - t) / 1000.0);
                    }
                    r.sink = x;
                    return nullptr;
                }, &runs[i]);
                pthread_attr_destroy(&attr);
            }
            for (pthread_t t : threads) pthread_join(t, nullptr);
            for (size_t i = 0; i < runs.size(); ++i) {
                std::vector<double> v = runs[i].rates;
                std::sort(v.begin(), v.end());
                if (v.empty()) continue;
                printf("thread %zu: %zu slices, M it/s min %.0f median %.0f max %.0f | slices:", i, v.size(), v.front(), v[v.size() / 2], v.back());
                for (double rate : runs[i].rates) printf(" %.0f", rate);
                printf("\n");
            }
            return 0;
        }
        if (argc > 2 && !strcmp(argv[1], "cpu")) {
            // cpu SECONDS [NAME...]: each process's CPU time over the next SECONDS (user+system, ms and % of one core), the busiest first;
            // for each NAME also its threads'. For finding which process limits a frame rate (run it during a swipe).
            static mach_timebase_info_data_t base;
            mach_timebase_info(&base);
            struct Proc {int pid; char name[64]; uint64_t ns;};
            auto taskNs = [&](int pid) -> uint64_t {
                ProcTaskInfo ti = {};
                if (proc_pidinfo(pid, kProcTaskInfo, 0, &ti, sizeof ti) != (int)sizeof ti) return 0;
                return (ti.totalUser + ti.totalSystem) * base.numer / base.denom;
            };
            auto threads = [&](int pid, uint64_t *ids, char (*names)[64], uint64_t *ns, int max) -> int {
                static uint64_t list[512];
                const int bytes = proc_pidinfo(pid, kProcListThreads, 0, list, sizeof list);
                int n = bytes > 0 ? bytes / (int)sizeof(uint64_t) : 0;
                if (n > max) n = max;
                int k = 0;
                for (int i = 0; i < n; ++i) {
                    ProcThreadInfo th = {};
                    if (proc_pidinfo(pid, kProcThreadInfo, list[i], &th, sizeof th) != (int)sizeof th) continue;
                    ids[k] = list[i]; memcpy(names[k], th.name, 64); names[k][63] = 0; ns[k] = th.userTime + th.systemTime; ++k;
                }
                return k;
            };
            static int pids[4096];
            static Proc before[4096];
            const int n = proc_listallpids(pids, sizeof pids);
            for (int i = 0; i < n; ++i) {before[i].pid = pids[i]; proc_name(pids[i], before[i].name, 64); before[i].ns = taskNs(pids[i]);}
            static uint64_t tid0[64][512], tns0[64][512]; static char tname[64][512][64]; static int tn[64]; static int tpid[64];
            int named = 0;
            for (int a = 3; a < argc && named < 64; ++a)
                for (int i = 0; i < n && named < 64; ++i)
                    if (!strcmp(before[i].name, argv[a])) {tpid[named] = before[i].pid; tn[named] = threads(before[i].pid, tid0[named], tname[named], tns0[named], 512); ++named;}
            // Per-core load: host_processor_info's tick counts (user, system, idle, nice) before and after the window.
            auto coreTicks = [](std::vector<uint32_t> &out) -> unsigned {
                natural_t count = 0; processor_info_array_t info = nullptr; mach_msg_type_number_t words = 0;
                if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &count, &info, &words) != KERN_SUCCESS) return 0;
                out.assign(info, info + words);
                vm_deallocate(mach_task_self(), (vm_address_t)info, words * sizeof(integer_t));
                return count;
            };
            std::vector<uint32_t> cpu0, cpu1;
            const unsigned cores = coreTicks(cpu0);
            // Thread states of the named processes, sampled every NGPU_CPU_SAMPLE_MS (default 20) ms while the window runs: how often
            // each thread was running (TH_STATE_RUNNING 1) against waiting (3, 4), and its current priority. One sample costs a
            // proc_pidinfo per thread, so it is limited to the named processes.
            struct Seen {uint64_t id; unsigned run, wait, other; int pri;};
            static Seen seen[64][512]; static int seenN[64];
            const double sampleMs = getenv("NGPU_CPU_SAMPLE_MS") ? atof(getenv("NGPU_CPU_SAMPLE_MS")) : 20;
            unsigned samples = 0;
            const double t1 = nowMs(), windowMs = atof(argv[2]) * 1e3;
            while (nowMs() - t1 < windowMs) {
                if (named && sampleMs > 0) {
                    ++samples;
                    for (int p = 0; p < named; ++p) {
                        static uint64_t list[512];
                        const int bytes = proc_pidinfo(tpid[p], kProcListThreads, 0, list, sizeof list);
                        const int k = bytes > 0 ? bytes / (int)sizeof(uint64_t) : 0;
                        for (int i = 0; i < k && i < 512; ++i) {
                            ProcThreadInfo th = {};
                            if (proc_pidinfo(tpid[p], kProcThreadInfo, list[i], &th, sizeof th) != (int)sizeof th) continue;
                            int j = 0;
                            while (j < seenN[p] && seen[p][j].id != list[i]) ++j;
                            if (j == seenN[p]) {if (j >= 512) continue; seen[p][j] = {list[i], 0, 0, 0, 0}; ++seenN[p];}
                            if (th.runState == 1) ++seen[p][j].run; else if (th.runState == 3 || th.runState == 4) ++seen[p][j].wait; else ++seen[p][j].other;
                            seen[p][j].pri = th.curpri;
                        }
                    }
                }
                const double left = windowMs - (nowMs() - t1);
                const double step = named && sampleMs > 0 ? sampleMs : left;
                if (left > 0) usleep((useconds_t)((step < left ? step : left) * 1e3));
            }
            const double ms = nowMs() - t1;
            coreTicks(cpu1);
            static Proc delta[4096];
            int m = 0;
            for (int i = 0; i < n; ++i) {
                const uint64_t now = taskNs(before[i].pid);
                if (now > before[i].ns) {delta[m] = before[i]; delta[m].ns = now - before[i].ns; ++m;}
            }
            std::sort(delta, delta + m, [](const Proc &a, const Proc &b) {return a.ns > b.ns;});
            printf("cpu over %.0f ms from %.0f us (ms, %% of one core):\n", ms, t1 * 1e3);
            if (cores && cpu1.size() == cpu0.size()) {
                printf("cores:");
                for (unsigned c = 0; c < cores; ++c) {
                    const uint32_t *a = &cpu0[c * CPU_STATE_MAX], *b = &cpu1[c * CPU_STATE_MAX];
                    const double user = b[CPU_STATE_USER] - a[CPU_STATE_USER], sys = b[CPU_STATE_SYSTEM] - a[CPU_STATE_SYSTEM],
                                 idle = b[CPU_STATE_IDLE] - a[CPU_STATE_IDLE], nice = b[CPU_STATE_NICE] - a[CPU_STATE_NICE];
                    const double all = user + sys + idle + nice;
                    printf(" %u:%.0f/%.0f", c, all ? 100 * (user + nice) / all : 0, all ? 100 * sys / all : 0);
                }
                printf(" (user/system %% per core)\n");
            }
            for (int i = 0; i < m && i < 25; ++i) printf("%8.1f %5.1f%%  %d %s\n", delta[i].ns / 1e6, delta[i].ns / 1e4 / ms, delta[i].pid, delta[i].name);
            for (int p = 0; p < named; ++p) {
                static uint64_t ids[512], ns[512]; static char names[512][64];
                const int k = threads(tpid[p], ids, names, ns, 512);
                struct T {uint64_t ns; const char *name; uint64_t id;};
                static T rows[512]; int r = 0;
                for (int i = 0; i < k; ++i) {
                    uint64_t was = 0;
                    for (int j = 0; j < tn[p]; ++j) if (tid0[p][j] == ids[i]) {was = tns0[p][j]; break;}
                    if (ns[i] > was) rows[r++] = {ns[i] - was, names[i], ids[i]};
                }
                std::sort(rows, rows + r, [](const T &a, const T &b) {return a.ns > b.ns;});
                printf("threads of %d (%u state samples: running/waiting %% of samples, priority):\n", tpid[p], samples);
                for (int i = 0; i < r && i < 12; ++i) {
                    const Seen *sn = nullptr;
                    for (int j = 0; j < seenN[p]; ++j) if (seen[p][j].id == rows[i].id) {sn = &seen[p][j]; break;}
                    const unsigned all = sn ? sn->run + sn->wait + sn->other : 0;
                    printf("%8.1f %5.1f%%  0x%llx %s | run %.0f%% wait %.0f%% pri %d\n", rows[i].ns / 1e6, rows[i].ns / 1e4 / ms, (unsigned long long)rows[i].id,
                           rows[i].name[0] ? rows[i].name : "-", all ? 100.0 * sn->run / all : 0, all ? 100.0 * sn->wait / all : 0, sn ? sn->pri : -1);
                }
            }
            return 0;
        }
        if (argc > 1 && !strcmp(argv[1], "ps")) {
            static int pids[4096];
            const int n = proc_listallpids(pids, sizeof pids);
            for (int i = 0; i < n; ++i) {char name[64] = {}; proc_name(pids[i], name, sizeof name); printf("%d %s\n", pids[i], name);}
            return 0;
        }
        if (argc > 1 && !strcmp(argv[1], "bundle")) {
            // Load the plugin the way Metal's discovery does, with every error visible.
            NSBundle *bundle = [NSBundle bundleWithPath:@"/System/Library/Extensions/NativeAdrenoMetal.bundle"];
            printf("bundle %s\n", bundle ? bundle.bundlePath.UTF8String : "nil");
            NSError *error = nil;
            const BOOL loaded = [bundle loadAndReturnError:&error];
            printf("load %d error %s\n", loaded, error ? error.description.UTF8String : "-");
            Class principal = bundle.principalClass;
            Class base = NSClassFromString(@"_MTLDevice");
            printf("principal %s subclass-of-_MTLDevice %d\n", principal ? class_getName(principal) : "nil",
                   principal && base ? [principal isSubclassOfClass:base] : -1);
            return loaded && principal ? 0 : 9;
        }
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
