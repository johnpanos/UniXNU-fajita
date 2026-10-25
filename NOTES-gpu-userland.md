# fajita GPU userland: Metal on the Adreno 630 under iOS 16.7.16 (20H392)

Goal: CARenderServer (backboardd) composites the system UI through a Metal device backed by the A630. The path is the same as sheng's
(docs/drivers/native-adreno.md in UniXNU): NativeMetal plugin -> metal2vulkan -> Turnip (Darwin KMD) -> IR3 -> NativeAdreno user client.
The kernel half (GMU, CP, SMMU, ring, fences, the published service) is in NOTES-gpu.md (branch gpu-port).

## Interface contract (agreed with the kernel side, live since run gpu7)

NativeAdreno's user-client ABI is unchanged (`drivers/NativeAdreno/NativeAdrenoABI.h`, version 1). On fajita:

| Item | Value |
| --- | --- |
| Direct service | `NativeAdrenoGpu` (class `NativeAdrenoGpuPublished`), open type 0, selector = ABI method; published by stage 10 (`fajita-gpu=10` or `kcmd gpu 10`) |
| Discovery service | `AGXAccelerator : IOAcceleratorES` with `MetalPluginName` = `NativeAdrenoMetal`, open type 0x4147, every method through the `'NTNL'` tunnel on one admitted selector (to be built, below) |
| chip_id | 0x06030002 (Turnip decodes gpu_id 630; Mesa's table matches 0x060300ff) |
| GMEM | 1 MiB at 0x100000 (= UCHE_GMEM_RANGE_MIN) |
| uche_trap_base | 0x1fffffffff000 |
| UBWC | highest_bank_bit 15, swizzle 0x6, macrotile 0, version 2 (Linux a6xx_calc_ubwc_config defaults; SDM845 UBWC 2.0) |
| flags | `NativeAdrenoDeviceClientIova`; not cached-coherent |
| VA | per-client windows below 4 GiB (render server 1 GiB at 0x20000000, two 256 MiB clients) |
| Timestamp | CP_ALWAYS_ON_COUNTER, 19.2 MHz, read inside GMU OOB_PERFCOUNTER_SET |
| Submit | a6xx framing in the kernel (CP_INDIRECT_BUFFER, CCU invalidates, CACHE_FLUSH_TS); 32-bit fences, at most 8 in flight |

## Turnip for the A630 (host)

Linux host build and check, in UniXNU branch `fajita-a630-userland` (johnpanos/UniXNU):

```sh
python3 scripts/build-turnip-linux-host.py          # pinned Mesa 85fda3b + recorded patches -> build/mesa-tu-linux
python3 scripts/check-turnip-darwin-linux-host.py --disasm
```

- `patches/mesa/turnip-darwin-linux-host.patch`: on a non-darwin host the darwin KMD takes IOKit/mach headers from the harness
  (`drivers/NativeAdreno/tests/hostsim/linux/include`); darwin builds are unchanged.
- The check preloads `libhostiokit.so` (the host NativeAdreno kernel stack over a software CP) and runs the Vulkan smoke and the plugin's
  own NativeTurnipSmoke sequence for `HOSTSIM_GPU=a740` and `a630`.
- Result: the A630 enumerates as "Turnip Adreno (TM) 630"; the plugin's fill, compute and render pipelines (20 programs) compile to
  a6xx gen1 IR3 (compute: `16 max_waves, 0 double_threadsize`). No shader executes on the host.

## 20H392 discovery findings

From the 20H392 dyld cache (scratchpad/dsc16) and kernelcache (.cache/ipsw/20H392/kernelcache.decompressed):

- Metal `_MTLRegisterDevices` is the iOS 17 chain: `IOServiceMatching("IOAcceleratorES")`, `IOServiceGetMatchingServices`,
  `_getMetalPluginClassForService` (MetalPluginName -> /System/Library/Extensions/<name>.bundle, MetalPluginClassName or the principal
  class, which must subclass `_MTLDevice`), `initWithAcceleratorPort:`, `_MTLAddDevice`.
- `_MTLDevice` is **528** bytes on 20H392 (536 on 21H461). NativeBridgeMetalFrontend.mm refuses a size mismatch, so it must accept 528
  on iOS 16 (fix pending in NativeMetal `fajita-a630`).
- IOGPUFamily 65.60.1: `IOAcceleratorES` is a 136-byte IOService subclass with a 168-entry vtable differing from IOService only in
  slots 0, 1, 7 (as on 21H461): `OriginalAcceleratorABI.h` holds. `IOGPU` is 704 bytes (736 on 21H461); the shim does not use it.
- Apple's personality: `AGXAcceleratorG10P_B0`, IONameMatch `gpu,t8015`, MetalPluginName `AGXMetalA11`.
- The plugin's iPadOS 17 diagnostics read and write fixed 21H461 QuartzCore addresses (`reportDisplayState`, `quartzCoreCallers`);
  they must be gated off on 20H392 (pending).

## Delivery

- The boot add-on never arrives (`A files=-1 ... mount=1/2`) because **the boot image's DarwinBoot predates GpuAddon support**:
  scratchpad/boot-kc.img's firmware volume has the `.trustcache` handoff but not `.gpuaddon`/`GpuAddon` (DarwinBoot d099354 adds it;
  Mu-Silicium/Build/fajitaPkg/.../DarwinBoot.efi is from 2026-10-21 23:13 without it). The payload carries kernel.macho.gpuaddon and
  the ADT has the GpuAddon placeholder; the old loader leaves it zero, so NativeRamBlock reports none. Fix: rebuild the boot image with
  the current DarwinBoot (keep the local `-v` change).
- Meanwhile the tools use the usb-debug agent's path: CI-signed binaries (`.github/workflows/gpu-tools.yml`), embedded in the kernel,
  written under /private/var/ngpu/sbin (seen as /usr/sbin with usb-debug=3) and run with
  `fajita-usbdbg-hub.py call run /usr/sbin/ngpu-probe`.

## Local build on Linux (instead of the CI round-trip)

`scripts/build-gpu-plugin-local.sh` builds the "GPU plugin (iOS 16)" artifact on the Linux host in the CI's layout, so
`make-fajita-gpu-addon.py` / plugin-addon.sh take it as a downloaded artifact. The CI workflows stay as the fallback.

```sh
B=/home/john/src/UniXNU-fajita-gpulocal     # any board worktree with this script; GPU_LOCAL_CACHE=$B/.cache shares the caches
$B/scripts/build-gpu-plugin-local.sh --unixnu /home/john/src/UniXNU-ios16-stack-gpuuser [--pack OUT.ngpu [--base BASE.ngpu]] [--tools]
# -> <unixnu>/build/gpu-plugin-local/{NativeAdrenoMetal.bundle,scanout_scale.metallib,adrenoprobed,*.plist,trust.json,sources.txt,codesign.txt}
#    --tools: <unixnu>/build/gpu-plugin-local-tools/{ngpu-probe,ngpu-metal,ngpu-vk} (gpu-tools.yml)
python3 -I $B/scripts/gpu-local/compare.py CI_ARTIFACT_DIR <unixnu>/build/gpu-plugin-local   # functional comparison table
```

How: UniXNU's own scripts (build-turnip-darwin.py --target ios, build-adreno-metal-plugin.py, build-native-metal-bridge.py) run
unchanged with `scripts/gpu-local/bin` first on PATH. Its `xcrun`, `clang`, `codesign`, `strip`, `nm`, `metal` and `metallib`
stand-ins come from `scripts/gpu-local/tool.py`:

- Compiler and linker: Fedora LLVM 22 clang + ld64.lld, `-target arm64-apple-ios16.0`, against the CI's own iPhoneOS 18.5 SDK
  (Xcode 16.4) and Xcode's `libclang_rt.ios.a`. The SDK's libc++ headers are used, never /usr/include/c++/v1, plus
  `-fno-sized-deallocation` (Apple clang's default).
- Compile-and-link calls are split: each source compiles on its own, in parallel, through ccache.
- Rust: 1.98.0 `aarch64-apple-ios`; cc-built pieces go through the same clang.
- Signing: rcodesign 0.29.0 (ad-hoc, 4 KiB pages, entitlements). CodeResources is written in codesign's format (byte-identical for
  the same files). `--verify` and `--display` use a built-in CodeDirectory check, because rcodesign's verify rejects every ad-hoc
  signature.
- metallib: scanout_scale.metallib comes from `.cache/gpu-local/metallib`, keyed by the sha256 of the .metal source and flags
  (a CI metallib, seeded with `--seed-metallib RUN_ID`). On a cache miss, for example after a shader change, the `metal` stand-in
  compiles the shader with Apple's Metal Developer Tools for Windows under Wine. It uses the CI flags (`-std=metal3.0
  -mios-version-min=16.0 -O2`, then `metallib`) and keeps the result in `.cache/gpu-local/metallib/wine/<key>.metallib`. A CI
  metallib always wins. Without Wine or the tools, a miss fails as before.
  - Setup: download the installer from developer.apple.com (Apple ID). Put it in the gitignored `.cache/metal-win/` and run
    `--setup-metal-win`. That installs the newest `Metal_Developer_Tools*Windows.exe` silently (NSIS `/S`) into the Wine prefix
    `.cache/metal-win/prefix-<version>`. Wine 11.0 runs it with no extra components. Never commit `.cache/metal-win`.
  - The 3.1 tools are metalfe-31001.660 (3.0 is 31001.643); CI's Xcode 16.4 is metalfe-32023.620. The output is deterministic
    and does not depend on the source path. It is not byte-identical to the CI metallib (3923 against 4483 bytes for the
    NativeMetal shader at key 54e45f63). Same: the metallib header (v2.7, iOS 16.0), the one function `scanout_scale` (kernel,
    AIR 2.5 / MSL 3.0), the argument metadata, the triple `air64-apple-ios16.0.0`, and every IR instruction (llvm-dis).
    Different, all from the compiler version: the producer string, the per-function hash, how argument no-alias is written
    (`noalias` against `"air-buffer-no-alias"`), function attributes, and 32023's TBAA, alias-scope and SDK Version metadata.
    Not yet run on the phone: seed a CI metallib for a build that ships.
- SDK: `--fetch-sdk [RUN_ID]` downloads the `Apple SDK export (encrypted)` artifact (.github/workflows/apple-sdk-export.yml:
  AES-256 with the repo secret SDK_TAR_KEY, kept for 1 day). It decrypts the artifact with ~/.config/unixnu/sdk_tar_key into the
  gitignored `.cache/apple-sdk`, then deletes the artifact. The repos are public: never commit or publish the SDK.

Times on this host:

| Build | Time |
| --- | --- |
| Cold build of everything (Mesa 700 steps, plugin) | about 70 s |
| First Rust bridge build | about 80 s |
| NativeMetal .mm change (rebuild, link, sign) | about 10 s |
| `--pack` | +1 s |

Turnip rebuilds only when the CI's mesa cache key changes. The tree restarts when tool.py, the compiler or the SDK changes.
Rebuilds are reproducible: the same inputs give the same CDHash.

Equivalence with CI run 40080075126 (same ref 8190417 and submodules). Results from `compare.py`:

- Identical:
  - linked dylibs and their versions;
  - LC_BUILD_VERSION (iOS, minos 16.0, sdk 18.5);
  - signature identifier, flags, CodeDirectory version, 4 KiB pages, blob slots, entitlements and validity;
  - Info.plist and every job plist (byte for byte);
  - CodeResources entries and rules;
  - scanout_scale.metallib (byte for byte, also as `__DATA,__scalelib`);
  - every non-Rust export.
- Differences, all from the toolchain:
  - Imports: Apple clang 17 uses objc_retain_xN/objc_release_xN, objc_claimAutoreleasedReturnValue, objc_opt_*,
    NSConstantIntegerNumber literals and ___chkstk_darwin stack probes. Upstream clang uses objc_retain, objc_release and
    objc_retainAutoreleasedReturnValue. lld lists dyld_stub_binder. All of these are in iOS 16's libobjc and libSystem.
  - Exports: 55 of 6644 Rust names differ. Identical-body functions (ash's per-extension loaders) are merged, and which name
    survives follows the crate hashes, which are derived from source paths.
  - Sections: ld-prime emits `__objc_stubs`, `__objc_methtype`, `__objc_intobj` and merges categories. lld emits `__literals` and
    `__objc_catlist`.
  - The local Turnip uses chained fixups instead of CI's dyld info. Sizes differ by -0.4% to +1.8% (adrenoprobed -12%).
  - LC_SOURCE_VERSION is absent locally.
- The tools (`--tools`) compare the same way against the "GPU userland tools" artifact.

## Fork branches

| Repo | Branch | Content |
| --- | --- | --- |
| johnpanos/UniXNU | `fajita-a630-userland` | Linux host Turnip build/check, Mesa host patch (on ios16-stack) |
| johnpanos/NativeAdreno | `fajita-a630` | Linux hostsim + A630 device info (fe43efc), kernel A630 changes (4ee23ca) |
| johnpanos/NativeMetal | `fajita-a630` | NativeTurnipSmoke watchdog guarded to Apple (080bb79) |
| johnpanos/UniXNU-fajita | `gpu-userland` | this note, package/gpu-tools (ngpu-probe), gpu-tools CI |

Local worktrees: /home/john/src/UniXNU-ios16-stack-gpuuser (UniXNU), /home/john/src/UniXNU-fajita-gpuuser (board).

## Hardware results (2026-10-23 sessions)

| Session | Result |
| --- | --- |
| gpuu2 | ngpu-probe: open (type 0x4147), DeviceInfo, a CP_MEM_WRITE IB from userspace read back, 200 submissions |
| session-193533 | stage 11: AGXAccelerator registered after the bound IOAcceleratorES matched the live metaclass |
| session-194706 | ngpu-vk: Turnip on the A630 passes fill 1024/1024, compute 1024/1024, render 512 px / 0 stray |
| session-195220 | panic pmap_assert_free: a killed client's BOs freed while still CPU-mapped; fixed (deferred close) |
| session-200101 | ngpu-metal: MTLCreateSystemDefaultDevice() = 'Turnip Adreno (TM) 630'; a blit fill on the GPU passes |
| session-201609 | backboardd takes the device, then SIGSEGV in QuartzCore's newPipelineLibraryWithFilePath:error: (upstream main answers it) |
| session-202908/210422 | backboardd aborts in create_fragment_shader: QuartzCore 20H392 sets a uchar3 constant the frontend refused (fixed) |
| session-211948 | **CARenderServer composites through Metal on the A630**: lock screen drawn by the native device, render pipelines compiled on the phone |
| session-213631 | **Boots straight into the GPU-composited lock screen** (stages at ~36 s, backboardd's first discovery takes the device) |

## Reproduce (GPU-composited boot)

```sh
scripts/fajita/build.sh --test                 # kernel (fajita_ios16_gpu_defconfig), plugin, add-on, payload, boot image
scripts/fajita/test/session.sh start           # boot it and keep it up; then session.sh run/push/pull/call
```
Add `ngpu-trace=1` (or any boot argument) to the build.sh command line. The boot image (`.cache/fajita-inputs/boot.img`) is
DarwinBoot d099354 + -v, which delivers the add-on.

On a first boot without the framebuffer preferences in Data, the plugin writes them and restarts backboardd once (marker 68f5).

Before fixing a failure, check UniXNU upstream (NativeMetal/NativeAdreno main, the GPU build scripts): upstream main is merged into the
fajita-a630 branches (NativeMetal, NativeAdreno 0f851b0) and UniXNU fajita-a630-userland 52c2a4f.

Session tools: `ngpu-metal ps|killall NAME|bundle`, `ngpu-vk`, `ngpu-probe` in /usr/sbin (pushed copies: run them from
/private/var/ngpu/sbin); boot args `ngpu-trace=1|2` (every ABI call, the plugin's markers and texts in the kernel log),
`fajita-gpu=11 fajita-gpu-delay=90` (stages after userland is up; earlier, stage 10 hung in early boot). The CRASH/ABORT rows of
BootProgress are in the live log. iOS 16 has no ls/echo/sysctl/kill/launchctl.

## Frame rate (2026-10-23, sessions 225259..232852)

Measured with on-demand swipes: `kcmd touch swipe up|down|left|right [FRAMES] | tap` (FajitaTouchProbe.cpp) and the host sampler
scripts/fajita/test/swipe-rate.py (fires each swipe through the hub, samples `kcmd gpu raw`: presented frames = hand-off completions,
per-submission ring with publish, completion latency and acquire). Correctness: lock screen after a backboardd restart and home screen
after a left+right re-render, compared with the all-skips-off baseline (below the clock / status bar).

| Change | Result |
| --- | --- |
| skips off (import 1 only) | publish 72-87 ms per submission, 1.7 fps |
| ro-skip + uncached-skip | publish 15-30 ms (import-skip 0) / 0.2 ms (import-skip 1), 2.3-2.9 fps |
| import-skip=1 | **wrong**: lock wallpaper red/black after a backboardd restart; default now 0 |
| acquire dedupe + wpub-skip (default on) | publish 0.2 ms, submit total 0.1-6 ms, 3.6 fps; screens identical |
| DDR/LLCC BCM vote (FajitaBusVote.cpp, fajita-ddr, on ios16-port) | memcpy 538 -> 3449 MB/s; **3.6 -> 8.9-9.9 fps** |
| GPU level with the vote: 257 / 520 / 710 MHz | 4.9 / 7.9 / 9.0 fps (GPU-bound); without the vote ~3 fps at any level |

Remaining per-frame cost: GPU execution (completion 70-90 ms per submission at 710 MHz, sysmem rendering forced by the plugin's
TU_DEBUG=sysmem,nocb for a7xx BV/BR hangs; the A630 has no concurrent binning) and an 7-10 ms acquire per completed fence on the
waiting thread. kcmd tools: `gpu raw|set|freq N|membw`, `bus [read|off|PEAK [AVG]]`, `touch ...`; frontend telemetry rows with
scripts/fajita/test/rows.py (readv of native_adreno_metal_rows).

## Left

1. Stages 10/11 before backboardd's first Metal discovery (today: `ngpu-metal killall backboardd` after stage 11).
2. The scanout path on fajita (NativeLegacyDisplay) for GPU-rendered frames.
3. Stage 10/11 early in boot (stage 10 hung at ~51 s uptime), so backboardd finds the device without a restart.
4. GetTimestamp inside a GMU OOB_PERFCOUNTER pair; the BO pool size.

## Frame trace and the worst frames (2026-10-24)

Per-frame timing of the render server's Metal stream, off by default (NativeMetal `FrameTrace.h`):

```sh
printf 'NGPU_FRAME_TRACE=1\n' > .cache/fajita-work/gpu-env-trace
scripts/fajita/test/session.sh push .cache/fajita-work/gpu-env-trace /private/var/ngpu/gpu-env 644
scripts/fajita/test/session.sh run /usr/sbin/ngpu-metal killall backboardd      # the new backboardd opens its device with the trace on
scripts/fajita/test/fps.sh
# the file: the "X frametrace PATH" line of /private/var/mobile/Library/Caches/com.apple.backboardd/m2v/pipelines.log
scripts/fajita/test/session.sh pull /var/mobile/Library/Caches/com.apple.backboardd/m2v/frametrace/backboardd-PID.csv .cache/fajita-work/ft.csv
python3 tools/gpu/frametrace.py .cache/fajita-work/ft.csv          # distribution, worst frames, ranked causes (worst vs median)
```

Lines: `C` per command buffer (made, commit, commit returned, translator kernel and GPU times, completion, work counts, pipeline
creation / waits / flushes while it was encoded), `W` waits, `P` pipeline/library/function creation, `S` per scanout swap (wait, offer,
copy returned, completed). The presenter's queue is labelled `ngpu.present`. GPU times are the translator's timestamps (uncalibrated on
Turnip: the duration is exact, the end is the completion seen). Per-pass GPU time: `MESA_GPU_TRACES=print_csv` and
`MESA_GPU_TRACEFILE=/var/mobile/Library/Caches/com.apple.backboardd/m2v/utrace.csv` in gpu-env, one swipe, then the plain gpu-env back
and the file pulled (1.5 MB a swipe), `scripts/fajita/test/passes.py`. Command capture for replay: `NGPU_CAPTURE=1 NGPU_CAPTURE_FRAMES=N`
(PipelineCapture.h; pull and replay with metal2vulkan m2v-census `scripts/pipeline-census/fajita-pull`, command-replay).

Findings (stack eded357, frame trace of 429 frames, u_trace of 135 frames):
- Frames are GPU-bound: the CA command buffer takes 27 ms of GPU, the presenter's scale 7.9 ms, back to back (median frame 35.6 ms).
  The GPU runs at 710 MHz during swipes (perf.py); SP is ~100% busy.
- Worst frames add CPU time on top: encode (incl. pipeline creation, up to 30 ms), the CPU gap before encoding, and the
  commit (vkQueueSubmit, ~6 ms a frame).
- u_trace per presented frame (22 ms): presenter scale 7.86 ms, CA full-screen pass 7.6 ms, shadow copy-ins (copy_buffer_to_image,
  BGRA surfaces 0.7-2.8 ms each) 4.2 ms, copy backs 1.0 ms.

Fixes:
- `scanout_scale4v` (NGPU_SCALE4V=0 opts out): 16-byte source and tap loads, scale 7.86 -> 3.70 ms; GPU 22.0 -> 19.0 ms a frame.
- NGPU_XR_AS_F16 left IOSurface-backed XR attachments with RGBA16Float pipelines: those passes refused their command buffers
  (App Library backdrop black). Pipelines now make a variant per combination of attachments a pass keeps XR on.
- Run-over-run slowdown in one boot (`scripts/fajita/test/leakwatch.sh`): the kernel's retire sweep grew 2.9 -> 14 ms a submission,
  because every completed job's acquireImported cleaned and invalidated every writable import of backboardd, and Vm::find scanned 4096
  slots per handle. NativeAdreno b9ab096: acquire only the imports a job named since their last acquire, and find by a hash index.
  Retire is 0.0 ms flat; App Library scroll 16 -> 31-33 fps; Settings scroll and home/App Library screenshots unchanged.
- `home-page1.sh --rebase` saves home-base/applib-base for the current Data (its layout changes as apps are installed and moved);
  `FAJITA_AUTORESET=1500 build.sh --test` gives sessions long enough for several fps runs.
- "N no-copy ... dealloc 0/0" is not a leak: in copy mode (NGPU_HOST_COPY) no-copy requests become owned copies, which are freed with
  their MTLBuffer; dealloc counts only real imports.
- u_trace caveat: MESA_GPU_TRACEFILE in gpu-env reaches every Metal process that starts while it is set, and the file keeps growing after
  the env is reverted (pulls then fail with size/crc mismatch or time out). Use a new file name per capture and pull it after the
  processes that opened it are gone; the frame trace (per-process file) has no such problem.
- Zero-copy DPU scan-out study: CA's offered frames are 1900-2600 physical runs over 11 MiB (kcmd gpu "frame layout"), and VIG0 scans
  the splash carve-out by physical address (MDP streams in bypass). Scanning CA's surfaces directly needs an apps_smmu context for the
  MDP streams (sdm845 mdss iommus 0x880/0xc80) with the frames mapped contiguously in IOVA, then VIG0 + QSEED3 per swap.
