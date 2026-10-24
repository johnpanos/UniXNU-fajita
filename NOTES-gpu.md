# fajita: Adreno 630 GPU bring-up (SDM845, a6xx gen1) — kernel handoff

Goal: hardware-accelerated rendering for iOS 16.7.16 (20H392, iPhone10,6 identity) on the OnePlus 6T, so
SpringBoard/CARenderServer composites through the Adreno 630 instead of the CPU. The iPadOS 17 stack (sheng, Adreno 740)
is the template. This note is the complete handoff of the **kernel side**, which is done and verified on hardware through
the published user client; the remaining work is userland (Turnip/NativeMetal) plus the AGXAccelerator shim.

Branch: `gpu-port` of the board repo (worktree `/home/john/src/UniXNU-fajita-gpu`). NativeAdreno is pinned to the shared
fork branch `johnpanos/NativeAdreno` `fajita-a630` @ `4ee23ca` (userland base `fe43efc` + my three kernel deltas).

---

## 1. Current state — what works, with run names

All ten kernel stages pass on hardware. Boot with `fajita-gpu=10` runs them automatically after `fajita-gpu-delay`
seconds; or send `kcmd gpu <n>` over the USB hub to drive them live.

| Stage | Proves | Run |
| --- | --- | --- |
| 1 read-only snapshot | GCC/GPUCC/SMMU/GMU registers readable; UEFI leaves gcc_gpu_cfg_ahb_clk on, GDSCs collapsed | gpu1 |
| 2 A630PowerCx | GPU clocks + gpu_cx_gdsc + gpu_cc_cxo up (CX domain powered) | gpu4 |
| 3 A630GmuClocks | gpu_cc_gmu_clk_src 200 MHz, gpu_cc_cx_gmu_clk running | gpu4 |
| 4 GMU SMMU context + votes | adreno SMMU bank 1 / SID 5 programmed and routed; command-DB ARC votes built | gpu4 |
| 5 A630GmuColdStart | **GMU/GPU power-up**: PDC/RSC, a630_gmu.bin into ITCM, CM3 released (FW_INIT 0xbabeface), HFI v1 fully exchanged (5 cmds, 0 errors, fw 0x10060000), SPTPRAC on (0x28) | gpu4 |
| 6 OOB GPU_SET | **GX on**: GX GDSC f8222000, RBBM_STATUS 0x1 (idle) | gpu5 |
| 7 GPU SMMU + SQE + hw_init + CP | **SQE running + CP ring**: SQE v0x207 (has WHERE_AM_I), A630HwInit, CP_ME_INIT (fence 1), CP_MEM_WRITE wrote 0x5e9a630 and signalled fence 2; no CP HW fault | gpu5 |
| 8 zap shader | zap (a630_zap.mbn) loaded via SCM PAS 13 (init_image/mem_setup/auth all 0); CP_SET_SECURE_MODE 0 — CP left secure mode under QHEE/TZ | gpu6 |
| 9 GPU interrupt | CP_MEMCPY of 1023 dwords matched 1023/1023 (GPU DMA through its 4K SMMU verified); GPU IRQ SPI 300 (INTID 332) fired, fence woke on it | gpu6 |
| 10 publish | **NativeAdrenoGpuPublished** registered (open type 0, direct selectors) with the A630 device info; CP timestamp timer attached | gpu7 |

Verified in iOS userland (gpu9, ioreg over USB): `NativeAdrenoGpu <class NativeAdrenoGpuPublished, registered, matched,
active>` under ArmDTPlatform — so a userland process can `IOServiceMatching("NativeAdrenoGpu")` and open it.

Display-safety (gpu8): shots at 150 s (before `gpu 10`) and 205 s (after) both show the lock screen fully rendered
(wallpaper, clock, camera button). Running all ten stages does **not** disturb scanout or the CPU-rendered UI — the GPU
SMMU is the dedicated adreno_smmu at 0x5040000, separate from the apps_smmu that display/UFS/IOSurface use. (An earlier
all-black shot at 220 s was just display sleep: no input had woken it.) The GPU bring-up is opt-in: the board default is
`fajita-gpu=1` (read-only stage 1), so a normal boot is unaffected.

AegirPoster / "Too many corpses" crash loop at ~80 s: **not GPU-related** — present in every run (PMIC, USB, RTC) with no
GPU activity. A pre-existing iOS 16 userland issue.

---

## 2. Hardware facts and quirks (found on fajita, each from a staged run or live USB peek)

- **gcc_gpu_cfg_ahb_clk** is CLK_IS_CRITICAL: UEFI leaves it on. The GPUCC/GX GDSCs are collapsed at boot.
- **GDSC ordering**: `gpu_cx_gdsc` must be powered *before* the voted GCC branches. `gcc_gpu_memnoc_gfx_clk` reads CLK_OFF
  until the CX domain is up (it is in the GPU_CX power domain). A630PowerCx enables the CX GDSC first, then the
  memnoc/axi branches (this was a real bug: gpu-port first run failed stage 2 with memnoc never running).
- **adreno SMMU S2CR quirk** (the big one): the SDM845 SMMU firmware has the Qualcomm quirk where a **FAULT-type S2CR
  write is ignored** and a **BYPASS write lands as FAULT** (arm-smmu-qcom `qcom_smmu_cfg_probe`). Confirmed live: writing
  bypass 0x00010000 to an S2CR read back 0x00020000. Reset S2CRs read 0x000200ff (FAULT, CBNDX 0xff), which
  `ArmSmmu4KAttachStreams` rejects as not-clean-FAULT (route Busy). FajitaGpuProbe stage 4 normalizes the GMU/GPU slots by
  writing BYPASS with CBNDX 0, which the firmware stores as the clean FAULT the kit leases. **SMR and translation-type
  S2CR writes stick; context-bank registers (CBAR, TTBR0, SCTLR) are writable.** Only FAULT-type S2CR writes are dropped.
- **GMU is legacy** (a6xx gen1): HFI v1, flat 32 KiB ITCM image (a630_gmu.bin, loaded straight into CM3_ITCM_START),
  SPTPRAC (CPU-controlled SP power), GX rail via BOOT_SLUMBER OOB, and the kernel programs the **PDC sequencer** itself
  (pdc_address_offset 0x30080, `gmu_pdc` 0x0b280000 / `gmu_pdc_seq` 0x0b480000). GMU chip id is massaged:
  0x06030002 -> 0x06030100 for GMU_HFI_SFR_ADDR. FW_INIT_RESULT = 0xbabeface on success. The A630 bw table is a single
  "off" entry ("the 630 GMU doesn't do bus scaling").
- **GMU payload sizes** differ from the A740: no icache (a page is still allocated but unused), 4 KiB dummy, 16 KiB
  debug/log/hfi, at a6xx_gmu.c addresses (debug 0x60400000, dummy 0x60000000, log/hfi after dummy). Set via
  NativeAdrenoGmuContextSetSizes.
- **GPU** uses VBIF (not GBIF), no hardware APRIV (so CP_RB_CNTL needs NO_UPDATE and the RPTR comes from CP_WHERE_AM_I),
  `VBIF_GATE_OFF_WRREQ_EN = 9`, UCHE ranges, a630_hwcg (RBBM_CLOCK_CNTL 0x8aa8aa02, gmu_cgc 0x00020202), a630_protect
  (32 entries), GMEM 1 MiB at 0x100000, UBWC 2.0 (hbb 15, swizzle 6), 4 GiB VA limit.
- **zap/SCM**: PAS id 13, SIP SMCs (STD/SMC64/SIP, svc 2: cmd 1 init_image ARGS(2,VAL,RW), cmd 2 mem_setup ARGS(3), cmd 5
  auth_and_reset ARGS(1)). Metadata (ELF header + hash segment) must be physically contiguous, 4K aligned, pushed to PoC.
  The region is gpu_mem 0x8c415000/0x2000 (sdm845.dtsi, no-map, below XNU memory). The OnePlus-signed a630_zap.mbn (path
  qcom/sdm845/oneplus6/) authenticates; CP_SET_SECURE_MODE 0 then succeeds. The SCM wrapper handles INTERRUPTED resume
  (x0==1, carry x6) and EBUSY (-12) retry.
- **GPU interrupt**: SPI 300 (sdm845.dtsi, level high) = INTID 332. Attached via armcpu_spi_attach; fires with status
  0x100000 (CP_CACHE_FLUSH_TS). A storm guard masks the GPU after 100000 empty interrupts.
- **CP always-on counter** (CP_ALWAYS_ON_COUNTER 0x0980, 19.2 MHz) is the timestamp source. Per the ABI contract it must
  be read inside a GMU OOB_PERFCOUNTER set/clear pair (a6xx_gmu_get_timestamp) — the current GetTimestamp reads it
  directly; **add the OOB pair** (see Open work) or it may read 0 / hang while GX is collapsed.
- Physical memory: gPhysBase 0x80000000, gPhysSize 0x300100000 (~12 GiB window; owned_memory_size 0x130000000 = 4.75 GiB).

---

## 3. Kernel code map

### drivers/FajitaAdreno (new, the A630's own power-up; freestanding C, host-tested)
- `A630Regs.h` — generated by `scripts/derive-a630-hwinit.py --linux <tree>` from the msm register XMLs and
  `a6xx_catalog.c` (a630_hwcg, a630_protect_regs). Dword offsets: `A630_*` GPU block, `A630_GMU_*` GMU, `A630_RSCC_*`
  RSCC (GMU + 0x23000), `A630_PDC_*` gmu_pdc / gmu_pdc_seq. The hwcg/protect tables are behind `A630_WANT_TABLES`.
- `A630Gmu.{h,c}` — `A630CmdDbFind`/`A630BuildPerfTable` (cmd-db ARC votes), `A630PowerCx`, `A630GmuClocks`, `A630RpmhInit`
  (PDC/RSC), `A630GmuColdStart` (the whole a6xx_gmu_resume cold path -> a6xx_gmu_fw_start -> a6xx_hfi_start_v1),
  `A630SetOob`/`A630ClearOob`, `A630GxOn`/`A630SptpracOn`, `A630SetFreq`, `A630HfiSend`. All go through `A630Io`
  (read32/write32/barrier/delay per block), so the host tests run the same code against a register model.
- `A630Gpu.{h,c}` — `A630HwInit` (the a6xx_gpu.c hw_init A630 branches through CP_SQE_CNTL=1), `A630SqeHasWhereAmI`, the
  PM4 packet helpers (`A630Pkt4`/`A630Pkt7` with odd parity, `A630CpInitWords` = CP_ME_INIT 8 dwords, `A630WhereAmIWords`,
  `A630FenceWords` = CP_SCRATCH + CACHE_FLUSH_TS, `A630SubmitPrologueWords` = CCU invalidates, `A630IbWords`).
- `tests/test_a630.c` — host tests of every sequence against a register model and a simulated GMU firmware (HFI v1 ACKs).
  Build: `cc -std=c11 -I drivers/FajitaAdreno drivers/FajitaAdreno/tests/test_a630.c A630Gmu.c A630Gpu.c && ./a.out`.
  Pass line: "A630 host tests: PASS (8249 GMU writes, 17 RSCC, 26 PDC, 193 GPU)". A real cmd-db dump (fjh readp
  0x85fe0000 0x20000 > db.bin) can be passed as argv[1] to check the vote derivation against the device.

### package/FajitaGpuProbe.cpp (the staged bring-up)
- Runs from the boot-progress monitor (`fajita_gpu_probe`, once a second; starts after `fajita-gpu-delay` polls when
  `fajita-gpu=<n>` is set) or the USB command `gpu`. `stageN()` functions 1..10 (and stage10 = publish). The runner
  thread advances one stage at a time; a failure stops and is reported, never panics.
- MMIO windows (`gWin[A630BlockCount]`, `gSmmu`) mapped once with `IODeviceMemory::map(kIOMapInhibitCache)`; `gIo` is the
  A630Io over them. `gmuRd`/`gpuRd`/`smmuRd` are direct helpers.
- Stage state: `gClocks`, `gPerf`, `gGmu`, `gHfi`, `gGmuMem` (CPU view of the HFI block, write-combined), `gGmuCtx`/
  `gGpuCtx` (the SMMU context reports), `gBuffers`/`gCpu` (the five GPU kernel buffers and their WC CPU maps), `gRing`.
- Zap (`stage8`): `scmCall` (raw `smc #0`), the MDT/ELF parse and segment copy into gpu_mem.
- IRQ (`stage9`): `gpuIrq`, `armcpu_spi_attach(332,...)`.
- Publish (`stage10`): `gSubmit.attach(vm,&gRing)`, `gTimer.attach(CP source)`, `native_adreno_publish(vm, &gSubmit,
  &gRing, &kClock, timer?nullptr:&gTimer, &gDeviceInfo, 32, output_bits)`. `gDeviceInfo` is the ABI struct (below).
- The USB `gpu` kcmd (`kcmd()`): `gpu N` runs stages 2..N, `gpu log`/`gpu regs`/`gpu retry`, `gpu` alone prints state.
- Firmware is embedded weak (`fajita_gpu_{gmu,sqe,zap}_fw[]`), from scripts/build-fajita-gpu.py's incbin of the pinned
  .cache files.

### The NativeAdreno deltas (fork branch fajita-a630, on top of fe43efc)
None touch NativeAdrenoABI.h.
- `NativeAdrenoGmuContextSetSizes(icache,dummy,debug,log,hfi)` — board-set GMU payload sizes before the bring-up.
- `NativeAdrenoGpuContextSetLayout(iovas,bytes)` — the five kernel buffers' GPU addresses above the A630 GMEM window.
- GPU context **single-stream** mode: `slots[1] == ~0u` attaches one route (SDM845 GPU: `iommus = <&adreno_smmu 0>`).
- `NativeAdrenoRing::setA6xx(true)` — A6XX (gen1) framing instead of A7XX: the fence is CP_EVENT_WRITE CACHE_FLUSH_TS|IRQ
  (event 4, no bit 27), framed submits get only the CCU depth/color invalidate prologue (no BR/BV thread control), and
  CP_RB_CNTL gets NO_UPDATE (no hardware APRIV). **The user client submits go through this path**, so it's the one the
  userland agent's Turnip submissions will exercise.

### How the ABI device info is filled (NativeAdrenoABI.h v1, the userland contract)
`gDeviceInfo` in FajitaGpuProbe.cpp, matching what the userland agent specified:
- chip_id 0x06030002 (msm/kgsl form, **not** the GMU-massaged 0x06030100)
- gmem_bytes 0x100000, gmem_base 0x100000 (= UCHE_GMEM_RANGE_MIN; must match A630Gpu.c's UCHE writes)
- uche_trap_base 0x1fffffffff000 (= UCHE_TRAP_BASE / WRITE_THRU_BASE)
- va_start 0x20000000, va_size 0x40000000 (render-server 1 GiB window, inside the 2 GiB context aperture, clear of GMEM)
- highest_bank_bit 15, ubwc_swizzle 6, macrotile_mode 0, ubwc_version 2 (match the RB/TPL1/SP/UCHE_MODE_CNTL writes)
- flags NativeAdrenoDeviceClientIova (not CachedCoherent — A630 BOs aren't IO-coherent)
- publish input_bits 32, output_bits from the context's ID2.OAS (36 on this SMMU).

---

## 4. Build, boot and drive

Scripts in the session scratchpad
(`/tmp/claude-1000/-home-john-src-UniXNU/1375c105-6991-4ff3-9a85-56683ac7fae0/scratchpad/`):
- `build-kernel16-gpu.sh` — fetches + merges origin/ios16-port, then `./unixnu build kernel` into out-ios16-gpu with the
  `fajita_ios16_gpu_defconfig` (adds libFajitaGpu.a to the force-load archives). Needs the xcrun shim on PATH for the
  archive step; the top-level unixnu build adds it, but if running build-fajita-gpu.py by hand prefix
  `PATH=/home/john/src/UniXNU-ios16-stack/scripts/unixnu/host:$PATH`.
- `build-payload16-gpu.sh [kernel-args...]` — payload into payload16-gpu.img. Adds the sheng-autoreset + wdstop backstop.
- `build-fajita-gpu.py` (in scripts/) — libFajitaGpu.a: the probe, A630 code, the NativeAdreno GMU/GPU/SMMU/ring objects,
  the ArmSmmu4K kit, and the three sha256-pinned firmware blobs (incbin from .cache/firmware). Skips sheng's A740 GMU
  sources (ADRENO_SKIP). The firmware is gitignored; ios16/.cache/firmware holds qcom/a630_gmu.bin, qcom/a630_sqe.fw,
  qcom/sdm845/oneplus6/a630_zap.mbn.
- Run on the phone: `scratchpad/cycle.sh gpuN payload16-gpu.img boot-kc.img` (USB log streams by default; queues on
  phone.lock). A ready-to-probe image that auto-publishes is `payload16-gpu-publish.img` (fajita-gpu=10, delay 8 s).

Boot args: `fajita-gpu=<n>` runs stages 1..n at boot; `fajita-gpu-delay=<s>` the delay before the thread starts (default
15); `fajita-gpu-level=<i>` the boot GX vote index; `fajita-gpu-zap=<0..4>` the zap depth (0 skip, 4 full + secure exit).
Keep `boot-progress=2`, `usb-debug=3`, and the backstop args.

Drive it live over the USB hub (the kernel log streams; `kcmd`/`read32`/`write32` reach the kernel):
```
hub=/home/john/src/UniXNU-fajita-ios16/scripts/fajita-usbdbg-hub.py   # or UniXNU-fajita-usbdbg/scripts/...
python3 -I $hub call kcmd gpu 10     # run stages 2..10
python3 -I $hub call kcmd gpu log    # the staged log
python3 -I $hub call kcmd gpu regs   # a register snapshot
python3 -I $hub call read32 0x5040800 5     # peek SMR slots (my MMIO peek, drivers/NativeDwc3/UsbDbgKernelTarget.h)
python3 -I $hub call run ioreg -c NativeAdrenoGpuPublished   # confirm the service in userland
```
The MMIO peek maps each 4 KiB page once with ml_io_map; reads of the GMU/GPU-core blocks are **power-guarded** upstream
(answer status -4 while gpu_cx/gpu_gx is collapsed — `kcmd power` lists the guarded blocks). GPUCC is never guarded. The
probe itself reads through its own mapped windows, not the peek path, so the guard doesn't affect bring-up.

---

## 5. Open work

- **AGXAccelerator shim** (the Metal-discovery path). The userland agent sent the 20H392 contract: same as iPadOS 17, so
  `drivers/NativeAdreno/NativeAdrenoAccelerator.cpp` and `NativeMetal/OriginalAcceleratorABI.h` apply unchanged.
  IOAcceleratorES (IOGPUFamily 65.60.1) is a plain 136-byte IOService whose 168-entry vtable differs from xnu-8796's
  IOService only in slots 0, 1, 7. Class name stays AGXAccelerator with newUserClient(task,securityID,type,properties,
  handler); registry MetalPluginName = "NativeAdrenoMetal", open type 0x4147 with the 'NTNL' tunnel through one admitted
  selector; keep NativeAdrenoGpuPublished beside it for direct opens. Apple's personality is AGXAcceleratorG10P_B0 with
  IONameMatch gpu,t8015 / MetalPluginName AGXMetalA11 — **ensure nothing on fajita matches gpu,t8015** so Apple's kext
  never loads. Metal's _MTLRegisterDevices matches IOAcceleratorES and reads MetalPluginName; the kernel needs nothing
  else. Gate it behind its own stage (11) / boot arg so the CPU UI stays default until the plugin works.
  **Blocker I hit:** the staged accelerator (build-native-adreno-accelerator.py) binds the base symbols
  `__ZN15IOAcceleratorESC2EPK11OSMetaClass` / `...D2Ev` / `...10gMetaClassE` from an ABI plan
  (scripts/data/ioaccelerator-base-abi.json) that was derived on **macOS via an HVF runtime observation**
  (audit-target-accelerator-base.py reads build/.../gdb-stop.json, pinned to the 21H461 parent metaclass 0xfffffff006aeb688).
  Those symbols are **not exported** in the 20H392 kernelcache, and the build script itself is macOS-only (xcrun/codesign/
  brew llvm). Two paths: (a) regenerate the plan + build the staged archive on the macOS CI (the way 21H461's was made) —
  safest; or (b) derive the IOAcceleratorES metaclass/ctor/dtor statically from the 20H392 IOGPUFamily __TEXT_EXEC on
  Linux and port the build like build-fajita-gpu.py did — faster but loses the runtime cross-check the audit uses to avoid
  a wrong vtable binding (which would crash the kernel when IOGPUFamily loads). I stopped before guessing. Also publish
  needs patch `zzw-native-adreno-sandbox-grant` so the render server's user-client open passes the sandbox
  (NativeSandboxGrantProcess on the accelerator), and backboardd's deep/compressed framebuffers must be turned off
  (NativeAdrenoMetal.publish beside the plugin bundle).
- **GetTimestamp OOB pair**: wrap the CP_ALWAYS_ON_COUNTER read in a GMU OOB_PERFCOUNTER set/clear pair
  (a6xx_gmu_get_timestamp), else it reads 0 / hangs while GX is collapsed. Currently it reads directly (fine while GX is
  held on during the staged runs, but not for a client's GetTimestamp after idle).
- **BO pool / per-client windows**: the pool is NativeAdreno's default; the contract says 512-768 MiB on fajita
  (sheng peaked at 782 MiB at 2.8K), settable from board args (native_adreno_pool_mib). The device info reports a single
  1 GiB render-server window at 0x20000000; the contract also wants two 256 MiB client windows below 4 GiB — the GPU SMMU
  context aperture is currently 2 GiB (GpuApertureBytes 0x80000000), so multi-client windows below 4 GiB need the
  aperture widened to 4 GiB (the A630 has the 4 GiB VA quirk; there is a `feat/gpu-aperture-4g` branch on the fork).
- **A6XX framed submit** path (setA6xx) is exercised only by the kernel self-tests so far; the user client's real Turnip
  IBs (CP_INDIRECT_BUFFER + CCU invalidates + CACHE_FLUSH_TS fence) are what the first userland probe will test. Keep
  a630_protect unchanged, one SMMU context, no per-process pagetable switch, no preemption/secure mode for user submits.
- **Unverified**: multi-submit / >1 fence in flight under a real client; the pool write-combine audit on A630; the
  display path accepting a GPU-rendered IOSurface (GpuScanoutClient) on fajita's NativeLegacyDisplay.
- **Ideas not reached**: letting a client's GetTimestamp drive GPU DCVS (A630SetFreq exists); a fast path that keeps GX
  on across submits vs. slumbering between frames.

---

## 6. Debugging tips — failure shapes and where to look

- `gpu` (no arg) prints `stage N running M failed F status X iofaults I`. `iofaults` > 0 means an MMIO access hit an
  unmapped/misaligned window (a window map failed). `gpu log` is the per-stage log; `gpu regs` a live snapshot.
- Stage 2 fail (`S2 clock ... memnoc 80004221 ... A630ErrClock`): a GCC branch never ran. memnoc stays off until the CX
  GDSC is up — check the GDSC enable precedes the branch waits.
- Stage 4 fail with `k0/10` (route_status 10 = ArmSmmu4KBusy): the S2CR normalization didn't take — the slot's S2CR
  isn't clean FAULT. Remember FAULT writes are ignored; you must write BYPASS (0x00010000) to get a clean FAULT. Check
  `S4 smmu slot N s2cr normalized to 00020000` appears. program_status (the `k0/...` first number) 0 = bank programmed.
- Stage 5 fail: `A630ErrFwInit` (FW_INIT_RESULT never 0xbabeface — GMU didn't come out of reset; check RSC/PDC and the
  ITCM load), `A630ErrHfiTimeout`/`A630ErrHfiReply` (no MSGQ interrupt or a bad/erroring reply — check gHfi.skipped /
  fw_errors / last_ret), `A630ErrRsc` (RSCC_SEQ_BUSY / CONTROL_ACK timeout). The `S5 hfi ...` row has the HFI counters.
- Stage 6 `gx OFF` / `A630ErrGx`: GPU_SET OOB acked but GX GDSC didn't power — check the GX vote index and that the GMU
  got a valid perf table (stage 4 votes).
- Stage 7: `cp_init` row shows rbbm/int/hwfault/rptr/wptr/shadow/fence. A stuck ring = rptr != wptr and fence not
  advancing; CP HW fault nonzero = bad packet or protect violation (check `protect` and CB0 FSR). SQE version must be
  >=0x190 (has WHERE_AM_I) or the RPTR shadow won't update without APRIV.
- Stage 8: `init_image a0` nonzero = TZ rejected the metadata (wrong hash/contiguity); check the zap region is outside
  XNU memory and the metadata PA == IPA. `auth_and_reset` nonzero = signature mismatch (wrong a630_zap.mbn — must be the
  OnePlus-signed one, not db845c's). A non-zero CP_SET_SECURE_MODE fence = the CP couldn't leave secure mode.
- Stage 9: `matched < 1023` = GPU DMA through the SMMU context is wrong (TTBR0 / SID routing); `irq attach` nonzero or
  `count` not growing = the GIC SPI isn't wired (ArmCpu) or the line number is wrong (332).
- Stage 10: `publish` nonzero = service init/attach failed. `opened/closed` track user-client opens. Confirm with
  `run ioreg -c NativeAdrenoGpuPublished`.
- Everything is gated + has the sheng-autoreset + wdstop + APSS-watchdog backstop, so a hung stage resets the phone to
  fastboot on its own; never design a run whose expected outcome is a hang without the backstop.

Host-test the register/GMU/HFI/PM4 logic before touching the phone (`drivers/FajitaAdreno/tests/test_a630.c`). The msm
reference is Linux v6.16 at `/home/john/src/ref/linux` (drivers/gpu/drm/msm/adreno a6xx_*, the register XMLs, gpucc/gcc
-sdm845.c, cmd-db.c); A630Regs.h is regenerated from it by scripts/derive-a630-hwinit.py.
