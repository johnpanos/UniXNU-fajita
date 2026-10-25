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

One branch per repository (2026-10-24 consolidation; frame-trace, vm-index, shader-perf and shadow-held-copyback were merged and deleted):

| Repo | Branch | Content |
| --- | --- | --- |
| johnpanos/UniXNU | `fajita-a630-userland` | the stack (`ios16/gpu-plugin.ref`): Linux host Turnip, the plugin build, pins |
| johnpanos/NativeAdreno | `fajita-a630` | kernel A630 changes, the VM handle index and acquire fixes (b9ab096) |
| johnpanos/NativeMetal | `fajita-a630` | the plugin and frontend, frame trace, pass trace, CPU profile, bridge patches (57bda1f) |
| johnpanos/metal2vulkan | `fajita-a630` | the translator, pinned in the stack's scripts/data/gpu-metal-vulkan-inputs.json (f372253f) |
| johnpanos/UniXNU-fajita | `ios16-port` | the board, this note |

After a translator pin bump, `python3 scripts/refresh-bridge-patches.py` (in the stack) rebases NativeMetal's recorded bridge patches
onto the new pin; build-native-metal-bridge.py refuses patches that apply only with offsets.

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

## Shader statistics on the host (2026-10-24)

`tools/gpu/shader-stats` compiles every render pipeline of a frontend journal (`pipelines.log`) with ir3 for the A630 on the Linux
host, no phone: metal2vulkan's pipeline-census (`scripts/pipeline-census`, branch `shader-perf`) through the runtime on Turnip's Linux
host build over the NativeAdreno host kernel stack, the phone's TU_DEBUG and the frontend's XR-as-RGBA16Float mapping. Per shader:
instructions, ALU (cat2+cat3), texture (cat5), memory (cat6), nops, (ss)/(sy) stalls, half/full registers, waves, the disassembly.

```sh
tools/gpu/shader-stats build                     # once, in FAJITA_SHADER_STACK (a UniXNU stack worktree; not the GPU agent's)
M2V=~/src/metal2vulkan-shaderperf tools/gpu/shader-stats run base     # QuartzCore/RenderBox metallibs in .cache/fajita-work/shader-stats/libs
tools/gpu/shader-stats top base --format 80; tools/gpu/shader-stats compare base new; tools/gpu/shader-stats disasm new r10
```

Findings (20H392 journal, 196 pipelines, 147 fragment shaders):
- Every unblended color write (and one/zero blends) into BGRA8Unorm or RGBA16Float rounded in the shader to Metal's exact code,
  computing both the unorm8 and the half rule and selecting by a rule word: +70-76 instructions a fragment. A sampled copy (wallpaper)
  was 85 instructions for one sample; a 27-tap blur 221. metal2vulkan `ColorWritePolicy::Device` (now the default, `Exact` opt-in):
  copy 85 -> 11, blur27 221 -> 146, blur15 179 -> 104 (24 -> 19 full registers, 8 -> 10 waves), weighted 31850 -> 20190 (-37%).

### Shader-perf handoff (2026-10-24)

Landed on johnpanos/metal2vulkan `fajita-a630` (fast-forwarded; `shader-perf` is the same commits; no wip branch):
- `77fb5b2c` pipeline-census (A630 host census, `scripts/pipeline-census`, `journal_manifest.py --xr-as-f16`).
- `fc4e10e9` `ColorWritePolicy::Device` default: no in-shader rounding of unblended unorm/half color writes (-70..76 instr a
  fragment). 147 journal states (XR as RGBA16F): 31850 -> 20190 instructions weighted by creations; wallpaper copy 85 -> 11.
- `b91baaa4` `ExtendedRangePolicy::Device` default: XR (BGRA10_XR...) decode/encode/half roundings in float arithmetic, note
  `DeviceExtendedRange`; `f372253f` the XR blend model's sums in f32 too. XR-kept states (161): 68072 -> 25909; the 11 heaviest
  13220 -> 2611 (source-over fill into BGRA10_XR 977 -> 158, 8 -> 6 full regs, 12 -> 16 waves). Exact forms: `DevicePolicy::exact_answers()`.
- Suite: no worse than base 5c4752a4 (pre-existing: golden_hashes table stale, air_input goldens, bind_shape_of,
  constant_blob_footprint, api_surface table/doc, engine_matmul2d order-dependent; runtime clippy items-after-test-module).
- Not yet measured on the phone (pin `f372253f` in the stack's gpu-metal-vulkan-inputs.json).

Running the census (`tools/gpu/shader-stats`):
```sh
tools/gpu/shader-stats build          # once: makes ~/src/UniXNU-shaderstats-stack (worktree of ~/src/UniXNU at ios16/gpu-plugin.ref,
                                      # submodules NativeAdreno NativeMetal ArmSmmu4K AdrenoPM4 AdrenoFirmware), host Turnip a630 + libhostiokit
cp <QuartzCore.metallib> <RenderBox.metallib> .cache/fajita-work/shader-stats/libs/   # 20H392's (a copy sits in ~/src/metal2vulkan-fajita/.cache/fajita-20H392)
tools/gpu/shader-stats run NAME [--exact-xr] [--only r1,r2]   # M2V defaults to ~/src/metal2vulkan-fajita (its translator is measured)
tools/gpu/shader-stats top NAME [--format 80]; tools/gpu/shader-stats compare A B; tools/gpu/shader-stats disasm NAME rID
```
Runs kept in .cache/fajita-work/shader-stats: `before` (5c4752a4), `after3` (f372253f), `base` (XR kept, 5c4752a4), `xr-after3`.

Findings for the next levers (A630 rules in NOTES-a630.md):
- Remaining XR fill (158 instr): fetch + unpack ~25, NaN/clamp of source and tile ~25, blend ~40, encode + pack ~45; a hand-written
  equivalent is ~35. Next: drop the NaN selects under the device policy (NanMinMaxPolicy::Native already allows the device's NaN),
  and fold the tile clamp into the decode (codes 0..1022 already bound the value).
- Half: 0 half registers everywhere. The translator holds `half` widened in f32 by design (lower/arith/legal.rs), and CA's `_lph`
  shaders do their math in float in AIR (NIR shows f2f16 only at outputs), so a whole-chain RelaxedPrecision would find little
  half ALU; forcing the existing F16 strategy gave +2.1% instructions, no register change. Re-check `x2` (double_threadsize) in
  `top` after any RelaxedPrecision prototype; ir3 wants >20% half ALU.
- Texture prefetch: none of the 147 shaders gets one. 50 single-sample shaders (fixed_frag Timg/Tcim, the icon and wallpaper draws)
  are otherwise eligible (coords straight from the varying, constant bindless handles) but sample with a bias operand (`txb`) whose
  value comes from CA's uniform buffer, so it is Metal's own bias, not the translator's; 77 `txl lod 0` (explicit level 0) samples
  could become implicit-LOD only for textures with one level (a variant key on the bound texture), not built.
- Occupancy: fixed_frag Bvcm variants sit at 19 full regs (10 waves); 16 regs would give 12. Blurs are 24 regs (8 waves) but cheap.

## Current state (2026-10-24, end of GPU round 2)

Pins: board ios16-port c9f4745 (+ this note), stack 93f2e41, NativeMetal 57bda1f, NativeAdreno b9ab096, translator f372253f.

fps.sh on a fresh boot (home plain / 60-frame, App Library scroll):

| Build | Home | App Library |
| --- | --- | --- |
| round 1 end (translator ad80f293) | 16-18 | 29-32 |
| + ColorWritePolicy::Device (fc4e10e9; stack 68008b8), 5 runs | 9.8-17.2 plain (one 48 outlier), median ~15 | 28.8-33.2, median 31.8 |
| + bridge fat LTO, one codegen unit (stack 8260d45 built with NMB_TARGET_CPU empty) | 17.1-17.5 / 17.0-18.9 (one 44 outlier) | **39.2 / 41.0 / 42.6** |
| + ExtendedRangePolicy::Device (b91baaa4, f372253f; stack 93f2e41) | built, not yet run on the phone | |

- Correctness of fc4e10e9: home maxdiff 1, App Library maxdiff 2, Settings scroll matches copyskip.
- `NMB_TARGET_CPU=cortex-a75` (the bridge tuned for Kryo 385) is wrong: the render server's surface imports failed with
  `surface import refused: no-copy buffer: OutOfMemory(Device)` in pipelines.log, and the App Library backdrop was black. LTO alone is
  correct. The knob stays in build-native-metal-bridge.py, off by default; the cause is not found.
- Home plain swipes sometimes read 44-48 fps. Check whether those swipes changed pages before trusting them.
- fps.sh found the App Library by the folders under the search field. Those reorder as the categories change with use, so it reported
  "not reached". It now compares the search field only (box 30,88,510,155).

### Per-pass GPU split (passtrace.sh, during fps.sh; ColorWritePolicy build)

GPU ms per presented frame, 450-850 swaps, about 20.5 ms in total:

| Pass | n/frame | ms/frame | median ms | p95 ms |
| --- | --- | --- | --- | --- |
| CA main composite rp 1125x2436 (10-99 draws, sysmem) | 1.05 | 7.7-8.8 | 4.9-13.7 | 14.7 |
| copy_buffer_to_image b8g8r8a8 (shadow copy-ins) | 3.9-4.0 | 3.8-3.9 | 0.69 | 2.7 |
| copy_buffer_to_image r8g8b8a8 | 4.3-6.5 | 0.8-1.2 | 0.21 | 0.22 |
| copy_buffer_to_image a8 | 9.2 | 0.5 | 0.07 | 0.16 |
| presenter scale cs 17x293 | 1.00 | 3.6 | 3.6 | 3.7 |
| copy_image_to_buffer b8g8r8a8 (copy-backs) | 2.0-2.2 | 1.4-1.6 | 0.71 | 0.8 |
| small full-screen passes (2-9 draws) | 0.7-0.9 | 0.55 | 0.45 | 1.2 |
| 0-draw load/store passes (64x32, 32x32, 64x128 a1 cl0/ld4/st4) | ~1.2 | 0.1 | 0.07 | 0.08 |

### CPU (frame trace + CpuProfile, home swipes, LTO not yet applied)

- Frame median 29.6 ms: cpu_gap 12.1 + encode 10.75 + commit 1.1 ms on the render thread, GPU 22.8 ms, gpu_queue 27.8 ms. CPU and
  GPU are about equal, so both need to come down.
- The 100-210 ms encode spikes in the first trace were **my own pass trace**: u_trace allocates its timestamp buffers per command
  buffer (tu_trace_create_buffer -> darwin_bo_init -> IOConnectCallMethod). Without it (`PASSES=0`) the spikes are gone. What remains
  are rare first-use pipeline creations (pipe up to 36 ms) and 200 ms gaps between idle 5 Hz frames.
- Render thread (t1) while encoding: the translator's draw path is 47% (draw_resident: settle_draw 18%, Recording::record 13%,
  bind::graphics::instantiate 9%, binding::bind 6%, ...: many small pieces, no single hotspot), end_pass 17% (shadow_copies 23% of
  it), malloc 10%, the render encoder's creation 6%. Blocked in encode: tu_cs_reserve_space -> darwin_bo_init (Turnip growing a command
  stream with a fresh BO through IOKit). Fat LTO took App Library from 32 to 41 fps, which confirms the CPU bound.

### Tools

- `scripts/fajita/test/passtrace.sh on|reset|pull|fps|off` (`CPU=1`: also the CPU profile; `PASSES=0`: without the pass trace).
  NativeMetal PassTrace.h feeds Turnip's u_trace through a pipe and aggregates it on the phone into
  `<cache>/frametrace/backboardd-passes.csv` (about 30 KB). Keys: render pass size, attachments, clear/load/store bytes, gmem|sysmem
  and draw bucket; transfers by format; with `TU_GPU_TRACEPOINT=+draw` in gpu-env, per draw by shader blake3. The pass trace adds CPU
  time to encode, so measure CPU without it.
- `tools/gpu/frametrace.py --passes FILE [--mesa-log LOG]`: the split. The Mesa log, with IR3_SHADER_DEBUG=fs,
  MESA_SHADER_CACHE_DISABLE=true and VK_ENABLE_PIPELINE_CACHE=false, names the draws' shaders.
- NativeMetal CpuProfile.h (`NGPU_CPU_PROFILE=backboardd`) samples the command-buffer threads about once a millisecond (suspend, frame
  walk, resume). It writes distinct stacks with enc/cpu/blocked counts to `backboardd-cpuprof.txt`.
  `tools/gpu/cpuprof.py FILE [--thread t1] [--only enc|cpu|all] [--under FUNC]` symbolizes against the unstripped images the plugin
  build now keeps (build/adreno-metal-plugin/*.unstripped; they match only the build that ran). The counts are relative: the real
  period is about 1-2 ms.
- `scripts/fajita/test/leakwatch.sh` (round 1): run-over-run slowdown in one boot.

### Next (in order)

1. Measure f372253f (stack 93f2e41): fps.sh x3, screenshot diffs within a few codes.
2. The CPU-time breakdown of backboardd per frame on home swipes, without the pass trace. Split it into CA's own work, the NativeMetal
   frontend, the metal2vulkan bridge (translate, encode, binding, pipeline), Turnip (vkCmd*, submit) and the kernel (ioctls, waits).
   Give the top 10 by self time and say whether pipeline compiles show up (so far: rarely, up to 36 ms on first use). Report the
   breakdown before fixing anything.
3. Translator per-draw CPU (draw_resident and end_pass above). Turnip command-stream BO growth: reuse command buffers and pools, or
   suballocate cs BOs, so encode never reaches IOKit.
4. Copy-ins: about 18 a frame (4 BGRA at 0.7 ms, 4-6 RGBA, 9 A8), 5 ms of GPU. List which surfaces and why the held-state skip
   misses them, before fixing.
5. Drop the 0-draw load/store passes (no draws and no clear: no pass at all; with a clear, DONT_CARE loads).
6. The lead's A630 items (NOTES-a630.md): LLCC slice activation for the GPU and SMMU walks (a6xx_llc_activate, behind a boot arg);
   GMEM through the autotuner (TU_AUTOTUNE_ALGO=profiled|prefer_gmem, TU_AUTOTUNE_FLAGS=tune_small); a TU_DEBUG=perf run for UBWC
   demotions; normalize perf.py's SP busy by the 2 SPs.
7. Shader next steps from the handoff above (XR fill NaN selects, texture prefetch, occupancy).

## Current state (2026-10-24, GPU round 3, in progress)

Measured on the stock first-boot Data (references rebased with home-page1.sh --rebase). The stock layout has two home pages and a
heavier App Library than round 2's Data, so the baseline moved: **home ~34 plain / ~31 60-frame, App Library ~19** (fps.sh, fresh
boot, x3). All variants are GPU-bound (gpu-ab: gpu busy 93-96%).

| Change | Home plain / 60-frame | App Library | Note |
| --- | --- | --- | --- |
| translator f372253f + stack 448b7b2 (Turnip cs-BO recycle, frontend string guard) | 34.3 / 31.1, 34.1 / 31.4, 33.6 / 31.3 | 17.6, 19.7, 19.6 | new baseline |
| NMB_TCACHE=1 (bridge thread-cache allocator) | 32.8 vs 32.3 (gpu-ab swipes) | | noise: GPU-bound |
| fajita-gpu-llc=3 (LLCC GPU + GPUHTW slices) | 34.1 / 31.9, 34.1 / 31.3 | 19.6, 18.4 | no gain: GPU BOs are outer non-cacheable |
| fajita-gpu-llc bit 2 (GPU MAIR idx0 outer WB) | broken | | backboardd crashed: CPU NC writes bypass the LLC; removed |
| TU_CS_RECYCLE=0 | 33.5 / 30.9 | 17.3 | recycle is not a regression |
| NGPU_COLOR_WRITES=exact + NGPU_EXTENDED_RANGE=exact | 31.8 / 30.6 | 16.1 | the round-2 shader policies help (ALU 66% -> 51%) |
| NGPU_YCBCR_SEED=0 + NGPU_HEAP_HELD=0 (no held shadows) | 21.2 / 20.6 | 7.2 | what the held-state skip is already worth |

Per-pass GPU split (passtrace.sh fps, stack 49d71e9; `.cache/fajita-work/pt-r3/split.txt`), 22.6 ms per presented frame: CA main
composite ~7.8 ms (sysmem, no GMEM), copy-ins 9.1 ms (BGRA8 4.7/frame 5.0 ms, RGBA8 15.7/frame 2.6 ms, A8 22.5/frame 1.5 ms),
presenter scale cs 3.65 ms, copy-backs 1.24 ms.

Copy-ins (translator `M2V_SHADOW_LOG=PATH` in gpu-env: one line per copy in with key, host|fixed, bytes, extent, reason, why=):
about 780 a second. By bytes: the 1125x2436 host surface "forgotten" (32 GB in 86 s, the stated generation unchanged), host
1271x1823 and 1326x1326 "unstated" (11.7 and 7.5 GB), and hundreds of small "fixed" (non-IOSurface) textures "unstated" (small in
bytes). "moved" (the CPU really wrote the surface): none. Fix attempts: surface generations stated at attachments and blits, private
heap textures held (stack 4c8694a: no change); surface views state their parents (next build). `M2V_SHADOW_UPPER_BOUND=1` measures
the prize (unsafe, measurement only).

Tools added this round: translator `encode_bench` example (host per-draw CPU: 5.8 us/draw on Zen 5, ~34k instructions and ~40
allocations a draw under callgrind); gpu-env A/B switches `TU_CS_RECYCLE=0`, `NGPU_COLOR_WRITES=exact`, `NGPU_EXTENDED_RANGE=exact`,
`NGPU_HEAP_HELD=0`, `NMB_TCACHE=1`, `NGPU_TEXTURE_LOG=1`, `M2V_SHADOW_LOG`, `M2V_SHADOW_UPPER_BOUND`; `gpu-ab.sh FPS=1` (fps.sh per
variant); `corr-tour.sh NAME` / `--diff A B` (Settings, App Library, lock, Notes, Calendar); perf.py per-SP ratios.

Known: Notes shows a black screen 4 s after launch (before and after these changes); not investigated.

### Copy-ins, continued (2026-10-25)

- Fixed (translator 8dae7705): a shadow's own copy back forgot its held contents (`StatedShadows::written` forgot every stated shadow
  over bytes a command writes, the copy back's own among them). CoreAnimation's 1125x2436 frame surfaces were copied in again at every
  frame whatever their IOSurface seed: 2,811 copy-ins and 32 GB in 86 s before, 11 and 0.12 GB after. fps did not move (34.4 / 32.0 /
  17.9, gpu busy 95%); the passtrace (pt-r3b) shows BGRA8 copy-ins 3.0/frame and 3.8 ms (from 4.7 and 5.0).
- Still copied in each use: two host textures of 1271x1823 and 1326x1326 (19 GB in 86 s, "unstated", import path not yet known; the
  log is shared by every Metal process, so the next log carries pid=), and CPU-written linear textures over Shared buffers
  (`newLinearTextureWithDescriptor`: 180x180 RGBA8 rows of 720 bytes, A8 labels, 256x1), whose bytes the CPU writes through
  `contents()` unseen, so their copy in is required.
- Turnip's a6xx linear layout needs a 64-byte row pitch even with an explicit layout (`fd6_layout.c`: pitchalign 6 for cpp <= 4;
  without an explicit layout cpp_shift + 6, 256 bytes for 4-byte texels). Rows of 720, 352, 288, 224 or 192 bytes cannot be an image
  over the buffer's bytes; only 64-byte-multiple pitches (128-byte rows, the 4608-byte frame surfaces) could be sampled in place.
- `M2V_SHADOW_UPPER_BOUND=1` (unsafe measurement): home 40.3 / 39.6, App Library 19.8, gpu busy 85%: the copy-ins are worth about +6
  on home and nothing on the App Library.

### Copy-ins fixed: CPU-held textures (2026-10-25)

**fps.sh x3, fresh boot, stock Data (board ios16-port, NGPU_CPU_HELD on): home 41.2 / 40.3 / 41.0 plain, 41.0 / 40.8 / 40.0
60-frame; App Library 25.6 / 25.1 / 26.3.** (Round-3 baseline on this Data: 34 / 31 / 18-19.) gpu busy 82-88%.

What landed (each with a gpu-env switch, A/B'd in one boot with `gpu-ab.sh FPS=1` and checked with `corr-tour.sh` against the switch off):
- Translator 8dae7705: a copy back no longer forgets its own shadow's held contents (CA's frame surfaces, 32 GB in 86 s).
- Translator 6d36a137: `replace_region` forgets a shadowed texture's held contents (why=7).
- NativeAdreno 32a2426 / 65368c0 / later: ABI method 17 `HostDirty`: per range of a live import, whether any of the import's
  pinned physical pages has its modified bit set (cleared by the query). Pageable pages are never examined. The caller's pmap does
  not map the imported pages at the imported addresses (every page read unmapped there), so it walks the import's spans.
  Turnip exports `tu_darwin_host_dirty`.
- NativeMetal `NGPU_CPU_HELD` (default on; `=0` off; `=2` also hashes copy-backed bytes per bind and journals "cpu held MISSED"):
  textures over no-copy buffers state a generation, so the runtime copies them into their shadows only when they changed:
  - over a GPU-owned copy (no-copy buffers up to 4 MiB, `NGPU_HOST_COPY`'s copy at creation): 1 + the buffer's write generation,
    moved by `replaceRegion` on any texture over the bytes, `didModifyRange`, purgeable Volatile/Empty; none once `contents` was
    asked for;
  - over an imported range (above 4 MiB: the wallpaper's images): moved when HostDirty says the CPU wrote it (once per command buffer);
  - only root textures, never views (a view's shadow copies from its parent's image); never a texture the GPU may write (render
    target, blit operand, compute texture: `noteGpuWritten`).
  Each rule was found by the tour: garbage dock icons (views), a Calendar strip (sibling textures over an atlas), the lock screen's
  camera glyph (a GPU-written texture).
- `M2V_SHADOW_LOG` (translator: copy-ins with reason, why=, pid, and why each texture became a shadow), `NGPU_TEXTURE_LOG`
  (creations by path), `NGPU_MESSAGE_LOG` (runtime messages), `M2V_SHADOW_UPPER_BOUND` (unsafe measurement).

Known, not from these changes: AegirPoster aborts (sig6) on lock-screen interaction; Notes opens black after 4 s; `kcmd key power`
hangs the hub call (corr-tour bounds it).

Next: the CA main composite (about 7.8 ms, all sysmem: GMEM autotune A/B on it), the presenter scale (3.6 ms; DPU scan-out
postponed by the user), and CPU per draw again now that gpu busy is below 90%.

### Async encode and the CPU side (2026-10-25)

**fps.sh x3, fresh boot, page 1 verified (pagematch.py): home 41.6 / 41.7 / 41.2 plain, 40.7 / 40.7 / 40.5 60-frame; App Library
29.8 / 30.9 / 30.2.** (Before async encode, same Data: 39.8 / 39.0 / 24.0.)

- Frame trace (busy frames, before async): 24.3 ms = cpu_gap 7.7 (the render server's own work) + encode 14.3 (about 121 draws,
  ~118 us a draw on the A75) + commit 0.85, serial on one thread; GPU 15.7 ms of CA's command buffers + 3.6 ms presenter, overlapped.
- NativeMetal `NGPU_ASYNC_ENCODE` (default on; `=0` off): every runtime call of an encoder and the runtime side of commit run on one
  serial USER_INTERACTIVE queue in call order; status/error/GPU times/waits/resource checks drain it; commit blocks at three pending.
  With it gpu busy is 93-95%: the GPU (about 19-20 ms a frame with the presenter scale) is the limit again.
- GMEM autotune A/B on top: `prefer_gmem` 31.4 / 33.4 / 28.3 (worse), `profiled`+`tune_small` 40.7 / 40.6 / 28.0 (same): sysmem stays.
- Translator per-draw CPU (host, `scripts/encode-bench-callgrind` in metal2vulkan): KeyMap for the bind memo's views and the use maps
  (36,085 -> 35,410 instructions a draw), inline push field bytes (39.4 -> 37.4 allocations a draw), stated shadows indexed by key.
  On the phone the render thread's self time: QuartzCore 19%, libsystem_malloc 9.7%, memmove 3.8%, the translator spread thin.
- `NMB_TCACHE=1` (bridge thread cache): +0.6..1.3 fps but a garbage dock icon in one run with CPU-held on: a use-after-free that the
  LIFO reuse exposes is suspected; stays off.
- One kernel panic seen once ("IOGMD: not wired for the IODMACommand", during a backboardd kill with tcache on and held off); an
  8-restart stress run on the default build was clean.

Next: the presenter scale (3.6 ms, DPU scan-out: the user's decision), copy-backs of CA's frame surfaces (1.3 ms), the composite.

### Per-draw GPU split after async encode (2026-10-25, passtrace TU_GPU_TRACEPOINT=+draw; Mesa log .cache/fajita-work/pt-r3f)

15.2 ms GPU per presented frame. One draw dominates: fs=0e6c0aededb80ab0 (vs=bd2a8fc2a9bba6f7), 0.77 a frame, 3.47 ms median each,
4.0 ms a frame (26%). Its fragment shader is 1079 instructions (2400 dwords): one `samb` of the main texture, `getsize`, and four
`isam.base0.1d` texel-buffer fetches from t#3 blended with 8-bit integer weights (256 - frac) — a texture buffer (likely the 256x1
RGBA16Float LUTs CoreAnimation makes, 283 creations in the texture log) read four times and interpolated per pixel, a full-screen
backdrop/vibrancy draw. Everything else is small per draw (fs=42c7... 18 a frame, 1.1 ms; fs=745f... 11 a frame, 1.1 ms).
Also measured: `M2V_ALIAS_PADDED_TARGETS=1` (render CA's frame surfaces in place) does nothing: their rows are 4512 bytes, not a
multiple of 64, so Turnip makes no linear image there.

### Default build, held + async (2026-10-25, stack with staleHeld)

**fps.sh x3, fresh boot: home 40.9 / 41.5 / 41.5 plain, 41.0 / 40.3 / 39.9 60-frame; App Library 30.7 / 31.8 / 29.7.**

- The intermittent garbage dock icon under NGPU_CPU_HELD (2 of 6 backboardd restarts) was a command buffer that never ran (released
  uncommitted, refused or failed) after the runtime had marked its planned copies into shadows held. NativeMetal `staleHeld()` moves
  the held generation of every root texture such a command buffer used. 13 of 13 restarts and a tour clean since.
- `NGPU_TRUSTED_RENDER_SERVER=1`: no gain (39.7 / 40.5 / 27.6).
- Seen once at a cold boot of this build: a black lock screen (no poster, no clock; swipe up did nothing); a restart of PosterBoard,
  SpringBoard and backboardd cured it with held on or off, and the next cold boot was fine. Not reproduced; watch for it.

### YCbCr surfaces on the device's linear filter (2026-10-25, translator 1d2b1097, stack 64cb192)

The 3.5 ms draw above (fs=0e6c...) is a linear sample of a YCBCR8_420_2P surface (1105x1160, row 1536). The translator emulated
Metal's filter on the 1/2048 grid in the shader (1079 instructions). The new `YCbCrLinearFilterPolicy::Device` (the bridge's default;
gpu-env `NGPU_YCBCR_FILTER=exact` keeps Metal's) samples the converted image with the device's filter instead.

**fps.sh x3, fresh boot: home 43.5 / 42.9 / 44.2 plain, 42.4 / 43.7 / 45.0 60-frame; App Library 29.3 / 29.0 / 29.1.**
Same-boot A/B, exact then device, twice each: home 40.9 / 40.7 vs 43.0 / 44.7; App Library 28.1 / 29.0 vs 28.3 / 27.9.
GPU busy on home fell from 94-96% to 61-64%, so home is no longer GPU-bound. corr-tour ycbcr16 vs held15 is within normal tour
variance; home and lock screenshots are clean.

### supportsFamily: memo (2026-10-25, translator 38a0567e, stack 59136df)

The CPU profile after the YCbCr change: 46% of backboardd's running samples were in Metal's `MTLGetTextureLevelInfoForDeviceWithOptions`
-> `supportsFamily:` -> `Device::supports_family` -> `families::audit`. Every texture's level layout re-ran the whole family audit,
cloning every format row's answers, plus the realloc/memmove of its rows Vec. The answer is now kept per device and family. Running
samples over the same run fell from 6689 to 3723.

**fps.sh x3, fresh boot: home 45.1 / 45.3 / 45.0 plain, 44.8 / 45.3 / 46.4 60-frame; App Library 34.7 / 35.1 / 33.3.**

Frame trace, median busy frame 21.9 ms = cpu_gap 7.7 + encode 2.9 + commit 9.6. The commit time is the async worker's translation:
CoreAnimation's `commitAndWaitUntilSubmitted` waits for it. GPU 10.6 ms overlaps, plus the presenter's 3.6 ms. The render thread runs
only about 27% of a core, and the rest is waiting. Next: `NGPU_SCHEDULED_EARLY=1` (A/B), so the wait ends once the commit is queued
on the worker.

### Where a swipe frame goes after the memo (2026-10-25, right swipe, frame trace C lines)

Steady state: CA commits its command buffer (about 120 draws, 6-12 passes). The encode worker finishes translating and submitting
it about 15 ms later, and the GPU runs it for about 14.6 ms. Then the presenter's copy takes 3.6 ms. Translation of frame N+1
overlaps the GPU work on frame N, so the period is 20-22 ms, which vsync quantization turns into about 45 fps (swap intervals cluster
at 16.7 and 33.3 ms).
- Neither half alone is the limit. `NGPU_SCHEDULED_EARLY=1` (CA's waitUntilScheduled returns before the worker submits) and the
  present-skip experiment (`NGPU_PRESENT_SKIP_FILE=1` plus a non-empty /private/var/ngpu/present-skip: frames handed back without
  their copy) both left the swipe rates unchanged (left 37, right 52).
- GPU: the pass trace accounts for about 11.1 ms of CA's passes and copies in a 14.6 ms GPU span; the other ~3.5 ms is not inside
  any traced pass.
- CPU: the worker's translation is about 125 us a draw (5.8 us on the Zen 5 host bench). With the worker sampled (the CPU profile
  now registers the encode worker's threads): draw_resident about 49% (record, settle_draw, instantiate, bind), end_encoding 11%,
  malloc 13% of all running samples.
- Cores: all 8 are online, and cpufreq-hw sits near the top of both clusters (little state 17 = 1766 MHz, big state 22 = 2650
  MHz). But drivers/ArmCpu registers every core as cluster 0 with the boot cluster's type, so XNU schedules without core types.
  `ngpu-metal spin`: a lone thread runs at 440 M it/s (big), and under 8-thread load slices fall to ~290 (the A55 rate for this
  latency-bound loop). A P cluster for cpus 4-7 is a kernel/board decision, raised with the lead.
- Swipe asymmetry (left 37 vs right 52 in swipe-rate.py) is mostly the window: in-motion frames are 20-22 ms both ways.
- Tools: `ngpu-metal cpu SECONDS [NAME...]` (per-process and per-thread CPU over a window) and `ngpu-metal spin`. Built on Linux
  through scripts/gpu-local's xcrun stand-in and pushed to /private/var/ngpu/sbin/ngpu-metal (run that path directly).

### The fps.sh window and motion fps (2026-10-25)

swipe-rate.py counts frames over the swipe plus SETTLE (1 s) after its lift, and the render server presents only what changes. On a
clean frame trace of one left:60 swipe, swaps come every 16-17 ms from touch to the animation's end at +1.03 s (three misses: 41,
33 and 26 ms), then only the idle Clock icon's 10 Hz frames. swipe-rate read 37.2 fps; a perfect 60 Hz animation would read about
35-38 there. swipe-rate.py and fps.sh now also print **motion fps**: frames over the longest stretch of submissions under 50 ms
apart.

**fps.sh x3 (default build): home 44.5 / 45.0 / 38.6 window, motion 55.1 / 54.7 / 46.0; App Library 34.1 / 34.9 / 32.9 window,
motion 51.8 / 54.3 / 53.6.**

- Clean frame trace (no pass trace: TU_GPU_TRACEPOINT=+draw serializes draws and showed 2 ms GPU gaps that a clean trace does not
  have, 0.03 ms): CA GPU 9.7 ms median (p90 14.1), worker commit-to-submitted 10.0 ms (p90 15.1), presenter 3.6 ms. Busy CA frame
  period median 17.4 ms.
- 7% of busy frames are heavy (10 passes, ~160 draws, 26.8 ms GPU) and miss vsync. Right swipes keep rendering about 2.2 s at ~53.
- GMEM again (`TU_DEBUG=nocb`, and `prefer_gmem`): 45.0 / 44.5 vs sysmem 45.1, no change.

### Inside the measured phases (2026-10-25, clean frame trace over one fps.sh run)

- Home phase: full-content frames (90+ draws) run at 56.8 fps, and 5% miss vsync (GPU ~13 ms on those). App Library phase:
  full-content frames run at 50.7 fps. Its 26-pass frames take 24-30 ms: worker translation 10-29 ms (median 14), GPU 9-15 ms.
- Most of the remaining 33 ms intervals in the motion numbers are content that changes at 30 Hz: ~44-draw frames with ~2 ms of
  GPU, which backboardd starts ~28 ms after the previous offer. Not a performance loss.
- The heavy frames (10-17 passes, ~13 Mpix, ~27 ms GPU) come only from home-page1.sh leaving the App Library, outside the
  measured windows.
- A/Bs with motion fps, all within noise (about ±2):
  - `NGPU_LINEAR_ALIGNMENT=64` + `M2V_ALIAS_PADDED_TARGETS=1`: CA's surfaces get 64-byte rows and render in place, and
    copy_image_to_buffer is gone (GPU per presented frame 8.7 ms on a swipe). Motion 54.5 / 55.0 vs 55.8 / 49.9.
  - `NGPU_SCHEDULED_EARLY=1`: 53.9 / 54.7 vs 54.7 / 53.9 home, 50.5 / 54.8 vs 54.3 / 52.8 App Library.
  - `NMB_TCACHE=1`: 55.2 / 54.9 vs 54.4 home, 50.7 / 54.9 vs 49.7 App Library.
- App Library profile (worker sampled): draw_resident 40% (settle_draw 18%, record 17%, instantiate 9%), end_encoding 13%,
  malloc 16.5% inclusive. GCD blocks (one per encoder call: _Block_copy/_Block_release) ~5%.
- The kernel config has no AMP (P/E cluster) support, and drivers/ArmCpu registers every core as one cluster: XNU cannot keep the
  render server's threads on the A75s.
