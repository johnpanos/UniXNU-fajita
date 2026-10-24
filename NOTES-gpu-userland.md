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

## Fork branches

| Repo | Branch | Content |
| --- | --- | --- |
| johnpanos/UniXNU | `fajita-a630-userland` | Linux host Turnip build/check, Mesa host patch (on ios16-stack) |
| johnpanos/NativeAdreno | `fajita-a630` | Linux hostsim + A630 device info (fe43efc), kernel A630 changes (4ee23ca) |
| johnpanos/NativeMetal | `fajita-a630` | NativeTurnipSmoke watchdog guarded to Apple (080bb79) |
| johnpanos/UniXNU-fajita | `gpu-userland` | this note, package/gpu-tools (ngpu-probe), gpu-tools CI |

Local worktrees: /home/john/src/UniXNU-ios16-stack-gpuuser (UniXNU), /home/john/src/UniXNU-fajita-gpuuser (board).

## Left

1. ngpu-probe on the phone (direct open, CP_MEM_WRITE read-back).
2. NativeMetal for 20H392: `_MTLDevice` 528, gate the 21H461 address diagnostics, iOS 16 deployment target.
3. CI: Turnip (darwin KMD, ios16 target), metal2vulkan bridge and the plugin bundle for arm64 iOS 16, signed.
4. The AGXAccelerator shim on the A630 kernel side, with MetalPluginName.
5. Delivery of the bundle (boot image with the current DarwinBoot, or the kernel-embedded add-on).
6. Discovery, the first GPU frame, then `--publish` for CARenderServer.
