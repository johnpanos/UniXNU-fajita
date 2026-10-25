# Adreno 630 (SDM845, a6xx gen1): shader and rendering optimization reference for metal2vulkan on Turnip

Scope: what makes Core Animation's composite passes fast or slow on the A630 under Turnip/ir3, for metal2vulkan's SPIR-V and
Vulkan output. The workload: full-screen 1125x2436 composites, mostly fragment shaders with sampling and blending, plus blurs. Frames
are GPU-bound (~24.6 ms), the main composite pass is ~8.3 ms, SP ~100% busy (perf.py), and every shader uses 0 half registers.

Conventions:
- **Fact** = stated in a cited source (Mesa source at the pinned commit, the Linux kernel, Qualcomm's docs, a measurement article).
- **Inference** = my reading or arithmetic on top of those facts. Every inference is labelled.
- Mesa citations are pinned to `85fda3b` (2026-06-30), the Mesa that `scripts/build-turnip-linux-host.py` builds and the phone runs
  (NOTES-gpu-userland.md). `M/` below stands for
  `https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/`.
- Source tags such as [M1] and [Q1] refer to the source list at the end.

Research date: 2026-10-24. No Qualcomm SDKs or binaries were downloaded. Everything below is links and short quotes only.

---

## 1. Specs table

| Item | A630 value | Status | Source |
| --- | --- | --- | --- |
| Generation | a6xx gen1 (`ADRENO_6XX_GEN1`), chip ids 0x06030001/0x06030002 (ours: 0x06030002) | Fact | [L2], NOTES-gpu-userland.md |
| GPU clock (OPPs) | 257, 342, 414, 520, 596, 675, 710 MHz | Fact (Linux DT) | [L1] |
| SP cores (Mesa "SP") | 2 (`num_ccu = 2`; "number of CCU is always equal to the number of SP") | Fact (Mesa) | [M2], [M3] |
| Wave size | base 64 fibers (`threadsize_base = 64`). Can double to 128 (`supports_double_threadsize = True`) | Fact (Mesa) | [M2], [M4] |
| Vulkan subgroup size | min 64, max 128. `requiredSubgroupSizeStages = ALL` | Fact (Turnip) | [M14] |
| Max waves per SP (ir3 units) | 16 (`max_waves = 16`). ir3 counts a wave *pair* as one wave (`wave_granularity = 2`) | Fact (Mesa) | [M3], [M4] |
| Register file budget | `reg_size_vec4 = 96` for gen1. Occupancy = `96 / (regs * (wave128 ? 2 : 1)) * 2`, capped at 16 | Fact (Mesa formula) | [M5] |
| Register file in bytes | not documented. If the formula's units are vec4 x 32 bit x 128 fibers, it comes to 192 KiB per SP | Inference | from [M5] |
| Half/full registers | "starting w/ a6xx, half regs conflict with full regs" (mergedregs): two half registers take one full slot | Fact (Mesa) | [M5] |
| FP32 vs FP16 ALU rate | ir3: in wave128, "Non-16bit ALU instructions go from 1 cycle per ALU op to 2, while 16-bit instructions run at double rate and still take 1 cycle" (tested on X1-85). Chips and Cheese on the A640: "a pair of 64-wide FP32 ALUs and a pair of 128-wide FP16 ALUs like other Adrenos". So FP16 is 2x **only in wave128** | Fact (Mesa, C&C A640). The A630 itself is not measured | [M5], [C2] |
| ALU lane count | Wikipedia: 256 [512] "SIMDs, FP32 (ALUs)", 727 GFLOPS FP32 at 710 MHz. Its row also lists FP16 = 727 GFLOPS, which contradicts the 2x above. 727 GFLOPS / (2 x 0.71 GHz) = 512 FP32 lanes per clock | Not documented by Qualcomm; Wikipedia is unsourced; the arithmetic is mine | [W1] |
| Texture units / rates | Wikipedia: 24 TMU, 17.04 GT/s, 11.36 GP/s | Unverified (Wikipedia only) | [W1] |
| SFU (EFU) rate | A640: special functions at 1/8 rate | Measured on A640 only | [C1] |
| GMEM (tile memory) | 1 MiB (`.gmem = SZ_1M`). Ours: 0x100000 at 0x100000 | Fact | [L2], NOTES-gpu.md |
| Tile limits | align 32x16, max 1024x1024, 32 VSC pipes | Fact (Mesa) | [M2] |
| CCU (render cache) | sysmem mode: 64 KiB color + 64 KiB depth per CCU. GMEM mode: CCU carved out of GMEM (1/4 fraction) | Fact (Mesa) | [M2] |
| TP L1 (texture) cache | Mesa: "1KB on A330, unclear how big now". C&C on the A640: "only has 1 KB of capacity like every other Adreno GPU I've seen" | A630 not documented | [M1], [C1] |
| UCHE (L2) | size not documented for the A630. A640: "128 KB L2 cache", ~47 cycles latency (measured) | A630 not documented | [C1] |
| System cache (LLCC) | SDM845 LLCC slices: GPU 2304 KiB, GPUHTW (GPU page-table walk) 512 KiB, both activated by Linux msm | Fact (Linux) | [L3], [L4] |
| DRAM | LPDDR4X, "4x16 bit up to 1866MHz". 4 ch x 2 B x 3733 MT/s = 29.9 GB/s peak | Fact (spec); bandwidth arithmetic is mine | [S1], [W1] |
| UBWC | UBWC 2.0, highest_bank_bit 15, swizzle 6. 8bpp UBWC supported (`has_8bpp_ubwc = True`) | Fact | NOTES-gpu-userland.md, [M2] |
| FS texture prefetch | yes (`has_fs_tex_prefetch = True`), up to 4 per shader (`IR3_MAX_SAMPLER_PREFETCH 4`) | Fact (Mesa) | [M2], [M8] |
| Const file | FS 512 vec4 (8 KiB) (`max_const_frag = 512`). Qualcomm: "There are 8k of constant memory per SP" | Fact (both agree) | [M6], [Q3] |
| Instruction preload | `instr_cache_size = 64` (in 128-byte units = 8 KiB = 1024 ir3 instructions). The real I-cache size is not documented | Fact for the preload size; I-cache unknown | [M2], [M3] |
| GPU SMMU | arm-smmu-v2 (`qcom,sdm845-smmu-v2`). Advertises 16K and 32M pages (16K is broken and hidden by Linux). Our NativeAdreno maps 4K pages | Fact | [L5], NOTES-gpu.md |
| TLB size / walk cost | not documented. C&C (A640): "memory latency skyrocket at larger test sizes, likely from TLB misses"; "staggeringly high past 64 MB" | A630 undocumented; A640 cause inferred by C&C | [C1] |
| LRZ | a6xx gen1: LRZ direction tracked on the CPU, not reusable across render passes (gen3+ tracks on the GPU) | Fact | [I1], [M1] |

## 2. Architecture notes

### 2.1 Shader core, waves and occupancy (the most important part for us)

- ir3 code is scalar. The compiler schedules all latency and sync by itself. `(sy)` waits on texture/prefetch, global loads and
  atomics. `(ss)` waits on SFU (cat4), local memory, shared/uniform register writes and shfl (`is_sy_producer` / `is_ss_producer`)
  [M9], [M10]. Fact.
- **Occupancy formula (fact, [M5] `ir3_get_reg_dependent_max_waves`):** `waves = reg_size_vec4 / (regs * (double ? 2 : 1)) *
  wave_granularity`, where `regs` is the highest full register (vec4) + 1 and half registers count half (mergedregs). On the A630
  (96, granularity 2, cap 16), wave64:

  | full vec4 regs | <=12 | 13 | 14-16 | 17-19 | 20-24 | 25-32 | 33-48 | 49-96 |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | ir3 waves | 16 | 14 | 12 | 10 | 8 | 6 | 4 | 2 |

  Wave128 halves these. This matches our shader-stats rows (24 regs -> 8 waves, 19 -> 10; NOTES-gpu-userland.md).
- **Wave128 choice (fact, [M5] `ir3_should_double_threadsize`, same at the pin and on main):** a fragment shader goes wave128 only if
  - it has "significant 16-bit ALU" (`full < half * 4`, i.e. more than ~20% of cat0-4 ops are half -> half) **and** `regs*2 <= 96`, or
  - it has no significant fp16 and `regs*2 <= 96/4`, i.e. **regs <= 12**.

  The reason given: on X1-85 the sweet spot for non-fp16 shaders was keeping >= 8 waves, and "For fp16 apps, the increased ALU rate
  made it worth it regardless." Inference: today every composite shader is fp32 with more than 12 regs, so all of them run wave64,
  and FP16 can't give its 2x rate in wave64.
- Wave128 also costs latency per request and divergence, but it halves the per-wave cost of uniform and scalar work and of icache
  misses (the [M5] comment list). Fact.
- Forcing: `IR3_SHADER_DEBUG=thread64` prefers wave64 [M7]. In Vulkan, `requiredSubgroupSize = 64` forces wave64 for any stage.
  Asking for 128 without `REQUIRE_FULL_SUBGROUPS` (compute only) leaves ir3's heuristic in charge [M14]. Fact.
- Qualcomm (blob-oriented, still useful): "Minimize usage of shader GPRs", "Unrolled loops tend to put texture fetches toward the
  shader top, resulting in a need for more GPRs", "Avoid uber-shaders ... this often increases GPR count" [Q2]. Fact (Qualcomm).

### 2.2 FP16 path through Turnip (what actually produces half registers)

Facts, all at the pin:
1. Turnip sets `.mediump_16bit_alu = true` ("Use 16-bit math for RelaxedPrecision ALU ops") [M11].
2. In spirv_to_nir, an ALU op whose **result id** carries `RelaxedPrecision` gets `f2fmp`/`i2imp` on its sources and `f2f32` on its
   result (`vtn_alu_op_mediump_16bit`). Back-to-back mediump ops then fold into a 16-bit chain. Derivatives are excluded unless
   `mediump_16bit_derivatives` is set, and so are bitfield ops [M12].
3. `RelaxedPrecision` on an OpVariable sets `GLSL_PRECISION_MEDIUM` [M12]. Function temporaries are lowered by
   `nir_lower_mediump_vars` [M11]. **FS inputs** (perspective and linear varyings, not flat) declared mediump become 16-bit loads
   (`nir_lower_mediump_io`), and FS outputs are then lowered "opportunistically" [M13].
4. Texture results become 16-bit when every use is an f2f16 conversion (`nir_opt_16bit_tex_image`, "Lowers texture operations that
   have only f2f16 or u2u16 called on them to have a 16-bit destination"), and 16-bit coordinates are used when they were only
   upconverted [M13].
5. `IR3_SHADER_DEBUG=nofp16` ("Don't lower mediump to fp16") turns steps 3 and 4 off for A/B tests [M7].

Hardware note (Qualcomm FAQ, fact): "After filtering the sample instruction, it will have a 16-bit per-component value for an 8-bit
texture ... what comes out of the texture pipe after filtering is still 16-bit", and "it does not cost anything to convert from half to
float" [Q3]. Sampling RGBA8 or BGRA8 into fp16 loses nothing.

Caution (inference): fp16 has an 11-bit significand. Texture coordinates on a 2436-texel axis need more precision than that (above
1024 texels, fp16 steps get coarser than a texel). Keep coordinates and position math fp32 and make color math fp16. This matches
Qualcomm's own advice to keep highp for "certain varyings, e.g., texture coordinates" [Q2].

### 2.3 Texture path

- **FS prefetch** (fact, [M8] `ir3_nir_lower_tex_prefetch.c`). Up to 4 samples are issued *before the shader starts*, and their results
  land in registers. A sample is eligible only if all of these hold:
  - it is `texture()` (`nir_texop_tex`, implicit LOD) with no bias, LOD, offset, gradient, projector, comparator or ms index;
  - 2D, not array, not sparse;
  - its coordinate is a varying used directly: a `load_interpolated_input` or a vec2 of consecutive components, pixel, centroid or
    sample barycentrics, smooth or noperspective, at a constant input slot;
  - it is in the shader's first block (after the preamble), in the entry point;
  - the texture index is <= 0x1f and the sampler <= 0xf, or bindless handles with constant indices < 65536.

  All prefetched samples share one interpolation mode (the most common one wins). Prefetch can write half-precision destinations
  (`half_precision`) [M15].
- Qualcomm: "Avoid random access – Hardware operates on blocks of 2x2 fragments"; dFdx/dFdy-based gradients "cost extra"; "Usage of
  four samplers in a single shader is acceptable" [Q2]. Fact (Qualcomm).
- Qualcomm: read-only SSBO/storage-image reads "effectively become texture fetches". Prefer UBOs that fit in the 8 KiB const RAM,
  because larger UBOs are "read directly from system memory by each wave without the benefit of the texture cache" [Q3]. Fact
  (Qualcomm, likely written for the blob).
- The TP L1 is tiny (1 KB on the A640 per C&C [C1]; size unknown on the A630 [M1]) and sits in front of UCHE. It is non-coherent with
  stores within a draw [M1]. Fact.
- Tiled versus linear (inference): Turnip forces linear for 1D images, swapped-format host-transfer images and some mutable cases
  ("No sense in tiling a 1D image, you'd just waste space and cache locality") [M16]. Imported IOSurface memory sampled as linear gets
  neither the tiled cache locality nor UBWC.

### 2.4 Render passes: GMEM (binning) versus sysmem (direct)

- Adreno "is a mostly tile-mode renderer, but with the option to bypass tiling ("gmem") and render directly to system memory
  ("sysmem")" [M1]. In sysmem mode, render target traffic goes through the CCU (64 KiB color per CCU on the A630) [M1], [M2]. Fact.
- Turnip's choice ([M17] `use_sysmem_rendering`, [M18]). Sysmem is used when:
  - `TU_DEBUG=sysmem` is set;
  - the attachments don't fit in GMEM, or the render area is empty;
  - tessellation, XFB or prims-generated queries need it;
  - there would be too many tiles without HW binning;
  - or the autotuner picks it.

  The default `bandwidth` algorithm compares
  `sysmem = per-pixel clears/resolves + draw traffic` with `gmem = (load+store bytes * 11 + draw traffic) / 10` ("The magic numbers
  of 11 and 10 are randomly chosen"). Render passes with fewer than 5 draws skip the autotuner and use the default mode (sysmem) unless
  `TU_AUTOTUNE_FLAGS=tune_small` is set. Fact.
- Qualcomm: avoid GMEM loads ("use LOAD_OP_CLEAR or LOAD_OP_DONT_CARE"), and "Remove unused render targets ... decreases the number of
  tiles". Blending is "not heavy on Adreno GPUs because of the graphics memory (GMEM)" [Q4], [Q2]. Fact (Qualcomm).
- **Our state: `TU_DEBUG=sysmem,nocb` is forced for every pass** (it was added for a7xx BV/BR hangs; the A630 has no concurrent binning)
  (NOTES-gpu-userland.md). So the A630 never uses GMEM today.
- Arithmetic (inference): a 1125x2436 target is 2.74 Mpx. At BGRA8 that is 11.0 MB, at RGBA16F 21.9 MB. One full read or write of it at
  the 29.9 GB/s peak takes ~0.37 ms (BGRA8) or ~0.73 ms (RGBA16F). For BGRA8 in GMEM, roughly 1 MiB minus the CCU carve-out gives
  >= 11 tiles. An ~8.3 ms pass is therefore not bandwidth-bound on its main target. GMEM's possible gain is in multi-layer blending
  (read-modify-write in GMEM instead of through the CCU), and its cost is a full load + store per pass with `LOAD_OP_LOAD`.

### 2.5 Compression (UBWC) and formats

- Qualcomm's A6X format table (fact, [Q5]): R8G8B8A8_UNORM, R10G10B10A2_UNORM, R16G16_FP16, R16G16B16A16_FP16 and R32G32B32A32_FP32 all
  support UBWC, fast clear and blending. R8_UNORM has no UBWC in that table. (Mesa enables 8bpp UBWC on the A630 anyway.) An
  `R10G10B10A2_UNORM_FAST` variant exists.
- Qualcomm FAQ (fact, [Q3]): 1010102 and 111110 formats "perform better than FP16", and "There is a hardware “fast path” for 1010102".
- Turnip UBWC eligibility (fact, [M16] `ubwc_possible` and image creation). UBWC is off for:
  - `VK_IMAGE_TILING_LINEAR`, compressed formats, E5B9G9R9, S8_UINT;
  - snorm (pre-a740), 3D images with mips;
  - storage images unless UAV UBWC is supported;
  - `HOST_TRANSFER` usage;
  - D24S8 with sampled stencil on the A630;
  - **`MUTABLE_FORMAT_BIT` without a UBWC-compatible format list** (and with swapped formats such as BGRA/RGBA, the image is also forced
    linear).

  `TU_DEBUG=perf` logs "Disabling UBWC ... due to mutable formats". Fact.
- Perf counters separate blending by type: `RB_BLENDED_FXP_COMPONENTS`, `RB_BLENDED_FP16_COMPONENTS`, `RB_BLENDED_FP32_COMPONENTS`, and
  texture filtering by width: `TP_FILTER_WORKLOAD_16BIT` / `_32BIT` [M19]. Inference: fixed-point, fp16 and fp32 blending and 16- and
  32-bit filtering take different paths. Their rates are not documented.

### 2.6 Constants, descriptors, push constants

- The FS const file is 512 vec4 (8 KiB) [M6]. ir3 has a preamble on a6xx (`has_preamble = true`) that hoists uniform math, plus shared
  registers [M6]. On gen 6 with `shared_push_consts`, push constants can sit at a shared const offset (504, 8 vec4) [M6].
  `IR3_SHADER_DEBUG=nopreamble` / `nouboopt` A/B these [M7]. Fact.
- Qualcomm: on A6X "push constants will perform better when used with frequently changing data". The blob recommends
  COMBINED_IMAGE_SAMPLER for its "Bindless mode" (separate samplers lost 2-5% fill rate) [Q3]. Fact for the blob. Inference: Turnip
  makes every descriptor bindless (`max_sets = 5` bindless bases [M2]), so the combined/separate split is a blob detail. Not measured
  on Turnip.
- Dynamic indexing: ir3 sets `force_indirect_unrolling = nir_var_all` on gen 6+ [M6]. Indirectly indexed arrays (for example
  temporary blur-weight arrays) are unrolled into selects or go to private memory (scratch). Fact for the option; the cost is
  inference.

### 2.7 SMMU, page size and physical scatter

- Facts:
  - The SDM845 Adreno SMMU "actually advertizes support for 16K (and 32M) pages", and 16K is broken, so Linux hides it [L5].
  - Linux activates two LLCC slices for the GPU: `LLCC_GPU` (2304 KiB) and `LLCC_GPUHTW` (512 KiB, page-table walks). It programs
    `CX_MISC_SYSTEM_CACHE_CNTL_1` with their SCIDs and `CNTL_0` for "not allocate cache lines on a write miss" [L3], [L4].
  - Linux maps page tables with `IO_PGTABLE_QUIRK_ARM_OUTER_WBWA` "to use system cache on behalf of the iommu page table walker"
    [L4].
- Fact (ours): `package/` and `drivers/NativeAdreno` contain no LLCC slice or SYSTEM_CACHE_CNTL programming. Only the RPMh BCM bus
  votes exist (FajitaBusVote.cpp).
- Inference: on our port every UCHE miss and every SMMU table walk goes to DRAM, with no system cache in between. Linux Turnip on SDM845
  also maps mostly 4K pages (shmem-backed BOs are scattered), so 4K scatter alone isn't unusual. The difference is the missing walk and
  data caching. The TLB size and miss cost are not documented. C&C's A640 latency cliff "past 64 MB" (TLB, inferred) is the only data
  point [C1].

## 3. Optimization rules for metal2vulkan on Turnip (ranked by likely impact for our workload)

Each rule gives: what to do, why, the source, and confidence (H/M/L). "Measure" names the counter or tool that would confirm it.

**R1. Emit RelaxedPrecision on color math, so the shaders reach the "significant fp16" threshold and wave128.** [M5], [M11]-[M13],
[C2], [Q1]. Confidence: H that this is the A630's only 2x ALU path; M on the size of the gain for our shaders.
- Today all shaders are fp32 with more than 12 regs, so they run wave64, where fp16 has no rate advantage (Mesa's comment). With
  more than ~20% of ALU ops in half (`full < half*4`) and regs <= 48, ir3 picks wave128, where fp16 ops take 1 cycle against fp32's 2.
- Put `RelaxedPrecision` on the result ids of color ALU ops: blend math, premultiply, color matrices, blur accumulation, and the
  ColorWritePolicy rounding.
- Also put it on the OpVariables for mediump varyings (colors, not UVs), and keep the decorated chains unbroken so the `f2fmp`/`f2f32`
  pairs fold away.
- Leave texture coordinates, positions, derivatives and anything indexed by pixel position at fp32.
- Measure: shader-stats half-register and waves columns plus the disassembly (`hr` registers and half ALU ops). Then
  `SP_FS_STAGE_HALF_ALU_INSTRUCTIONS` against `FULL_ALU`, A/B with `IR3_SHADER_DEBUG=nofp16`, and `passes.py` for pass time.
- Correctness: screenshot diffs. Blur sums of 27 taps in fp16 can band. Accumulate in fp32 if needed and keep only the per-tap math
  in fp16.

**R2. Cut full-register pressure to cross the occupancy steps (12/16/19/24 regs).** [M5], [Q2]. Confidence: H on the mechanism; M on
the gain (it depends on how latency-bound the pass is).
- The table in 2.1 is exact for ir3. Going from 19 to 16 regs gives 10 -> 12 waves, and <= 12 regs gives 16 waves (and wave128 even
  for fp32).
- Half registers count half, so R1 also lowers `regs`.
- Avoid emitting large temporary arrays (they get unrolled or scratched), and avoid unrolling loops that hoist many samples to the top
  (Qualcomm: unrolling "put[s] texture fetches toward the shader top").
- Measure: shader-stats waves and regs; perf.py `sp_stall_tp` (a higher wave count should lower it).

**R3. Make the first <= 4 samples of each FS prefetchable.** [M8], [M15], [Q2]. Confidence: H on the rules; M on the gain (it hides the
first fetch's latency and frees the shader prologue).
- Sample with plain `OpImageSampleImplicitLod` and no ConstOffset, Bias, Lod or Grad, at the top of `main`, outside any branch.
- The coordinate must be a vec2 varying passed through untouched: no `uv*scale+bias` in the FS, no swizzle reordering.
- For blurs: compute up to 4 tap coordinates in the vertex shader as separate varyings, and keep the remaining taps as normal samples.
  Metal's `sample(s, uv, offset)` becomes ConstOffset, which disqualifies prefetch, so fold the offset into the VS-computed varying.
- Measure: the disassembly shows the prefetch block (`num_sampler_prefetch` / prefetch entries); `SP_STALL_CYCLES_TP`.

**R4. Benchmark GMEM against sysmem per pass instead of forcing sysmem on the A630.** [M1], [M17], [M18], [Q2], [Q4]. Confidence: M.
The gain for an ALU-bound pass is uncertain.
- `TU_DEBUG=sysmem` came from a7xx hangs and is not needed on the A630 (no concurrent binning).
- A/B with `gpu-ab.sh`:
  - default (`bandwidth` autotune);
  - `TU_AUTOTUNE_ALGO=profiled`;
  - `TU_AUTOTUNE_ALGO=prefer_gmem`, plus `TU_AUTOTUNE_FLAGS=tune_small` (CA passes have few draws, so the autotuner otherwise skips them).
- For the Vulkan side, use `LOAD_OP_CLEAR`/`DONT_CARE` wherever Metal's loadAction allows (each LOAD costs a full GMEM load), and avoid
  `STORE` on transient attachments.
- Expected (inference): GMEM helps passes with many blended layers over the same pixels, and passes whose targets would otherwise go
  through the CCU and DRAM. It does not reduce SP ALU time.
- Measure: `passes.py` per pass, and RB/CCU stall counters (`RB_STALL_CYCLES_CCU*`).

**R5. Keep render targets and sampled intermediates UBWC-eligible, and prefer 32-bit formats over RGBA16F where the range allows.** [Q3],
[Q5], [M16]. Confidence: M.
- Create metal2vulkan-owned intermediates (backdrops, blur ping-pong, shadow copies) as `OPTIMAL` tiling.
- Don't set `MUTABLE_FORMAT_BIT` unless a view really reinterprets the format. If it does, pass a `VkImageFormatListCreateInfo` with
  only UBWC-compatible formats; BGRA and RGBA swaps force linear.
- Don't add `HOST_TRANSFER` or `STORAGE` usage unless needed.
- Check with `TU_DEBUG=perf` (it prints every "Disabling UBWC" decision) and `TU_DEBUG=layout`.
- Qualcomm states 1010102 and 111110 beat FP16. The current XR -> RGBA16Float mapping doubles bytes per pixel, and probably the filter
  and blend cost (fp16 blend and 32-bit filter paths; inference).
- For surfaces whose content fits [0,1], keep 8-bit. Only XR content needs the wider format.
- Measure: `TP_FILTER_WORKLOAD_16BIT/32BIT`, `RB_BLENDED_FP16_COMPONENTS`, `UCHE_VBIF_READ_BEATS_TP`.

**R6. Turn on the GPU system cache (LLCC GPU and GPUHTW slices) in the kernel, as Linux does.** [L3], [L4]. Confidence: M that Linux
depends on it; L on the size of the gain. Neither we nor anyone else has measured it on the A630.
- This is the most concrete SMMU/scatter lever: page-table walks and UCHE misses would hit a 2.3 MiB + 512 KiB cache instead of DRAM.
- Physical scatter of 4K pages by itself is the normal state on Linux too (inference).
- If done, also map texture BOs with 64K/2M pages where they are physically contiguous (the SMMU supports them; 16K is broken [L5]).
- Measure: perf.py `vbif latency` (`UCHE_VBIF_LATENCY_CYCLES/SAMPLES`), `tp stall on uche`, and pass times before and after.

**R7. Avoid patterns ir3 handles badly.** [M6], [M5], [Q2]. Confidence: M.
- No dynamically indexed local arrays: gen 6+ unrolls every indirect, and large ones spill to scratch.
- Keep branches uniform. Push-constant or specialization-constant branches are cheap; per-pixel divergence runs both sides ("both
  branches will be taken").
- Fewer distinct ALU-op types per shader (uber-shaders) mean fewer registers.
- Prefer built-ins to hand-written math; Qualcomm calls built-ins "optimized for specific shader profiles".

**R8. Put uniform data in push constants or small UBOs, and let the preamble hoist uniform math.** [M6], [Q2], [Q3]. Confidence: M.
- Keep per-draw uniforms under 8 KiB with static offsets, so ir3 promotes them into the const file.
- Large or dynamically indexed UBOs fall back to loads.
- Uniform-only math is hoisted into the preamble automatically, so don't precompute it at the cost of more varyings or registers.
- Measure: `IR3_SHADER_DEBUG=nopreamble,nouboopt` A/B in shader-stats (instruction counts).

**R9. Discard and FS depth writes: low impact for CA.** [Q2], [Q3], [I1]. Confidence: H that it's minor here.
- Discard and depth writes disable LRZ and early-Z, which only matter with a depth attachment. CA composites have none, so discard
  costs only its own instruction and the kept helper lanes.
- Still prefer alpha-0 blending to discard in hot shaders; Qualcomm: "discard operations can disable hardware optimizations".

**R10. Prefer UBO and texture reads over SSBO and storage-image reads for read-only data.** [Q3], [M1]. Confidence: M.
- Storage reads go through the texture path anyway, and storage usage can cost UBWC (R5).

## 4. Measurement tools and counters

### 4.1 Environment knobs (Turnip / ir3, at the pin)

| Knob | Use | Source |
| --- | --- | --- |
| `IR3_SHADER_DEBUG=fs,disasm` | print FS disassembly, including the `max_waves`/`double_threadsize` header line | [M7], [M5] |
| `IR3_SHADER_DEBUG=nofp16` | turn off mediump -> fp16 lowering (A/B for R1) | [M7] |
| `IR3_SHADER_DEBUG=thread64` | prefer wave64 (A/B wave128) | [M7] |
| `IR3_SHADER_DEBUG=nopreamble,nouboopt,nodescprefetch` | A/B for R8 | [M7] |
| `TU_DEBUG=sysmem` / `gmem` / `nobin` / `forcebin` | force a render mode (A/B for R4) | [M1], [M20] |
| `TU_AUTOTUNE_ALGO=bandwidth\|profiled\|prefer_sysmem\|prefer_gmem`, `TU_AUTOTUNE_FLAGS=tune_small,big_gmem` | autotune control. Docs: prefer_* is "better than using TU_DEBUG=sysmem/gmem when done for performance reasons" | [M1] |
| `TU_DEBUG=perf` | perf warnings, including every UBWC/tiling demotion | [M16], [M21] |
| `TU_DEBUG=noubwc`, `nolrz`, `layout` | A/B UBWC, LRZ; print image layouts | [M20] |
| `TU_DEBUG=perfc` / `perfcraw` | expose `VK_KHR_performance_query` (counter queries per command buffer) | [M14] |
| `TU_DEBUG_FILE=path` | toggle a subset of TU_DEBUG flags at runtime (sysmem, gmem, nobin, nolrz, perf, ...) | [M1] |
| `VK_KHR_pipeline_executable_properties` | Turnip always exposes it: per-pipeline ir3 statistics at runtime | [M14] |
| `MESA_GPU_TRACES=print_csv` (u_trace) | per-pass GPU time (we use it via `passes.py`) | NOTES-gpu-userland.md |

### 4.2 a6xx counter groups (fact, [M19] `a6xx_perfcntrs.json/.xml`)

Groups and counter slots: CP 14, CCU 5, TSE 4, RAS 4, LRZ 4, CMP 4, HLSQ 6, PC 8, RB 8, SP 24, TP 12, UCHE 12, VFD 8, VPC 6, VSC 2 (plus
RBBM and VBIF, which perf.py already uses). Countables most relevant to us:

- SP: `BUSY_CYCLES`, `ALU_WORKING_CYCLES`, `EFU_WORKING_CYCLES`, `STALL_CYCLES_TP/UCHE/RB`, `WAVE_IDLE/NOP/WAIT/FETCH/SHORT_SYNC/LONG_SYNC_CYCLES`,
  `FS_STAGE_FULL_ALU_INSTRUCTIONS`, `FS_STAGE_HALF_ALU_INSTRUCTIONS`, `FS_STAGE_TEX_INSTRUCTIONS`, `FS_STAGE_EFU_INSTRUCTIONS`,
  `ICL1_REQUESTS/MISSES`, `LOW_EFFICIENCY_STARVED_BY_TP`, `GPR_READ/WRITE_CONFLICT`, `EXECUTABLE_WAVES`, `PIXELS_KILLED`.
- TP: `BUSY_CYCLES`, `STALL_CYCLES_UCHE`, `LATENCY_CYCLES/TRANS`, `L1_CACHELINE_REQUESTS/MISSES`, `OUTPUT_PIXELS(_POINT/_BILINEAR/_MIP)`,
  `FILTER_WORKLOAD_16BIT/32BIT`, `QUADS_OFFSET`, `QUADS_GRADIENT`, `STARVE_CYCLES_SP/UCHE`.
- UCHE: `BUSY_CYCLES`, `STALL_CYCLES_ARBITER`, `VBIF_LATENCY_CYCLES/SAMPLES`, `VBIF_READ_BEATS_TP/SP`, `READ_REQUESTS_TP`, `GMEM_READ_BEATS`.
- RB: `BUSY_CYCLES`, `STALL_CYCLES_CCU*`, `C_READ/C_WRITE`, `BLENDED_FXP/FP16/FP32_COMPONENTS`, `3D_PIXELS`, `BLENDER_WORKING_CYCLES`.
- CCU: `COLOR_BLOCKS/BLOCK_HIT`, `PARTIAL_BLOCK_READ`, `GMEM_READ/WRITE`.

Mesa's perfetto producer derives (fact, [M22] `fd_pps_a6xx.cc`):
- "Shader Core Utilization" = `SP_BUSY / (time * max_freq * num_sp_cores)`
- "% Time ALUs Working" = `SP_ALU_WORKING / 2 / SP_BUSY`
- "% Shader ALU Capacity Utilized" = `(FULL_ALU + HALF_ALU/2) / 64 / SP_BUSY`
- "% Texture L2 Miss" = `UCHE_VBIF_READ_BEATS_TP / 2 / UCHE_READ_REQUESTS_TP`
- "ALU / Fragment" = `(FULL + HALF/2) / HLSQ_QUADS`

Several of these are tagged "(?)" in the source.

**Caveat for perf.py (inference, should be checked):**
- perf.py prints `sp busy = SP_BUSY / RBBM_ALWAYS` and `alu = SP_ALU_WORKING / SP_BUSY`, with no `/num_sp_cores` and no `/2`.
- If, as Mesa's formulas suggest, `SP_BUSY` sums both SPs and `ALU_WORKING` counts two ALUs, then "SP ~100%" could be ~50% per SP,
  and the ALU % is doubled.
- Check: if `sp_busy` can exceed `rbbm_always`, it sums across SPs. Compute the perfetto-style ratios before deciding whether the pass
  is ALU-bound or latency-bound.
- Suggested perf.py additions: `SP_FS_STAGE_HALF_ALU_INSTRUCTIONS`, `SP_FS_STAGE_FULL_ALU_INSTRUCTIONS`, `SP_ICL1_MISSES`,
  `TP_FILTER_WORKLOAD_32BIT`, `RB_BLENDED_FP16_COMPONENTS`, `SP_EXECUTABLE_WAVES`.

### 4.3 Tools

- `fdperf` (src/freedreno/perfcntrs/fdperf.c) is a curses counter monitor for Linux. It maps the GPU via the device tree and
  /dev/mem, so it can't run on iOS. Our `kcmd gpu perf` / perf.py is the equivalent [M19].
- Perfetto: data sources `gpu.counters.msm` and `gpu.renderstages.msm` (the pps-producer) [M23]. Linux only.
- Mesa `computerator` `test-regfile.sh` measures the register file and wave granularity on real hardware. It could verify the 96-vec4
  budget on the A630, but it needs the msm DRM [M24].
- Qualcomm's Snapdragon Profiler (Rendering Stages, % CP Busy, Texture L1 miss) works only with the Qualcomm Android driver. It is
  not usable on our stack [Q2], [Q3].

## 5. Open questions

1. A630 ALU lane count and the real FP32/FP16 rate. Qualcomm publishes nothing. Wikipedia's 512 lanes and its "FP16 = FP32" are
   unsourced and conflict with Mesa and C&C. A micro-benchmark (an fp32 against an fp16 FMA loop in a wave128 FS) would settle it.
2. Whether `SP_BUSY_CYCLES` and `SP_ALU_WORKING_CYCLES` sum over both SPs and both ALUs (section 4.2). This decides whether "SP ~100%
   busy" means ALU-bound or latency-bound.
3. TP L1, UCHE and TLB sizes on the A630: undocumented (A640 numbers only, from C&C).
4. Texel rate for 64-bit (RGBA16F) against 32-bit formats, and the cost of fp16 against fixed-point blending: undocumented. Measure with
   `TP_FILTER_WORKLOAD_*` and pass times.
5. Whether LLCC slice activation is possible from our kernel (it needs the LLCC broadcast registers that `llcc-qcom` programs), and
   what it gains.
6. GMEM correctness on our stack: `TU_DEBUG=sysmem` was put in for a7xx hangs. The A630's GMEM path hasn't been run on this port.
7. How many CA intermediates are IOSurface-backed (linear, no UBWC) as opposed to metal2vulkan-owned (could be tiled + UBWC).
8. The ALU-op mix that `uses_significant_16bit_alu` sees after R1 (cat0-4 only, half -> half ops). Conversions (`cov`) count as full
   ops, so too many precision boundaries can keep a shader below the 20% threshold.

## 6. Sources

Mesa (pinned 85fda3b; `M/` = `https://gitlab.freedesktop.org/mesa/mesa/-/blob/85fda3b36405bcaea8bffc64aa4f2651dc90770d/`):
- [M1] docs/drivers/freedreno.rst (hardware architecture, cache hierarchy, TU_DEBUG, autotune): https://docs.mesa3d.org/drivers/freedreno.html and `M/docs/drivers/freedreno.rst`
- [M2] A630 device entry and a6xx_base/gen1 props: `M/src/freedreno/common/freedreno_devices.py#L413` (a6xx_base #L129, a6xx_gen1 #L178)
- [M3] dev-info field meanings (wave_granularity, max_waves, num_sp_cores): `M/src/freedreno/common/freedreno_dev_info.h`
- [M4] defaults `threadsize_base = 64, max_waves = 16`: `M/src/freedreno/common/freedreno_dev_info.py#L140`
- [M5] occupancy, wave128 heuristic, 16-bit ALU test, mergedregs accounting: `M/src/freedreno/ir3/ir3.c#L109` (`uses_significant_16bit_alu`), `#L165` (cycle comment), `#L196` (`ir3_should_double_threadsize`), `#L323` (`ir3_get_reg_dependent_max_waves`), `#L94` (half regs)
- [M6] const-file limits, preamble, shared push consts, force_indirect_unrolling: `M/src/freedreno/ir3/ir3_compiler.c#L250-L305`, `#L394`
- [M7] IR3_SHADER_DEBUG options: `M/src/freedreno/ir3/ir3_compiler.c#L19-L46`
- [M8] FS texture prefetch rules: `M/src/freedreno/ir3/ir3_nir_lower_tex_prefetch.c`
- [M9] (ss)/(sy) producers: `M/src/freedreno/ir3/ir3.h` (`is_ss_producer`, `is_sy_producer`)
- [M10] legalize pass: `M/src/freedreno/ir3/ir3_legalize.c`; ir3 notes: https://docs.mesa3d.org/drivers/freedreno/ir3-notes.html
- [M11] Turnip spirv options and mediump var lowering: `M/src/freedreno/vulkan/tu_shader.cc#L42`
- [M12] spirv_to_nir RelaxedPrecision handling: `M/src/compiler/spirv/vtn_alu.c` (`vtn_alu_op_mediump_16bit`), `M/src/compiler/spirv/vtn_variables.c` (`apply_var_decoration`)
- [M13] ir3 FS mediump IO and 16-bit tex/image folding: `M/src/freedreno/ir3/ir3_nir.c#L1003`, `#L1028`, `#L1666`
- [M14] Turnip subgroup sizes, required-subgroup-size mapping, pipeline_executable_properties, perfc: `M/src/freedreno/vulkan/tu_device.cc#L1084`, `#L256`; `M/src/freedreno/vulkan/tu_shader.cc` (wavesize key)
- [M15] prefetch half precision: `M/src/freedreno/vulkan/tu_shader.cc` (`prefetch->half_precision`), `M/src/freedreno/ir3/ir3.c` (sampler_prefetch accounting)
- [M16] UBWC/tiling eligibility: `M/src/freedreno/vulkan/tu_image.cc#L316` (`ubwc_possible`), `#L585` (mutable formats)
- [M17] sysmem decision: `M/src/freedreno/vulkan/tu_cmd_buffer.cc#L1384` (`use_sysmem_rendering`)
- [M18] autotune bandwidth algorithm and small-RP rule: `M/src/freedreno/vulkan/tu_autotune.cc#L1029`, `#L1806`; per-pixel bandwidth: `M/src/freedreno/vulkan/tu_pass.cc` (`tu_render_pass_bandwidth_config`)
- [M19] a6xx counters: `M/src/freedreno/registers/adreno/a6xx_perfcntrs.json`, `M/src/freedreno/registers/adreno/a6xx_perfcntrs.xml`; fdperf: `M/src/freedreno/perfcntrs/fdperf.c`
- [M20] TU_DEBUG flags: `M/src/freedreno/vulkan/tu_util.cc#L22-L60`
- [M21] perf_debug: `M/src/freedreno/vulkan/tu_util.h`
- [M22] perfetto derived counters: `M/src/freedreno/ds/fd_pps_a6xx.cc`
- [M23] Mesa perfetto docs: https://docs.mesa3d.org/perfetto.html
- [M24] computerator register-file test: https://fuchsia.googlesource.com/third_party/mesa/+/main/src/freedreno/computerator/examples/test-regfile.sh
- ISA XML (reference): `M/src/freedreno/isa/`; a6xx registers: `M/src/freedreno/registers/adreno/a6xx.xml`

Linux:
- [L1] SDM845 GPU OPP table: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/arch/arm64/boot/dts/qcom/sdm845.dtsi
- [L2] A630 catalog entry (GMEM SZ_1M, ADRENO_6XX_GEN1): https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/msm/adreno/a6xx_catalog.c
- [L3] SDM845 LLCC slices (GPU 2304, GPUHTW 512): https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/soc/qcom/llcc-qcom.c
- [L4] a6xx LLC activation and page-table-walker system-cache quirk: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/msm/adreno/a6xx_gpu.c (`a6xx_llc_activate`, `IO_PGTABLE_QUIRK_ARM_OUTER_WBWA`)
- [L5] "Work around SDM845 Adreno SMMU w/ 16K pages" (Konrad Dybcio, 2024): https://lists.infradead.org/pipermail/linux-arm-kernel/2024-July/946237.html
- Split pagetables for Adreno (background): https://lore.kernel.org/all/20190827171342.GA29932@bogus/t/

Qualcomm (Game Developer Guide; the old static pages are cited through the Wayback Machine because the current docs.qualcomm.com pages render client-side):
- [Q1] Overview (scalar architecture, "twice the performance ... mediump", UBWC, FlexRender, LRZ): https://web.archive.org/web/20220428074640/https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/developer-guide/gpu/overview.html ; current: https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/overview.html
- [Q2] Best practices: shaders: https://web.archive.org/web/20211011232929/https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/developer-guide/gpu/best_practices_shaders.html ; current: https://docs.qualcomm.com/bundle/publicresource/topics/80-78185-2/mobile_best_practices.html
- [Q3] FAQ (const RAM 8k, SSBO vs UBO, push constants on A6X, combined samplers, 1010102 fast path, 16-bit filter output, LRZ): https://web.archive.org/web/20211011233004/https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/developer-guide/gpu/faq.html
- [Q4] Best practices: tiling (GMEM loads, subpasses): https://web.archive.org/web/20211011232956/https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/developer-guide/gpu/best_practices_tiling.html
- [Q5] Spec sheets (A5X/A6X texture format table: UBWC, fast clear, blendable): https://web.archive.org/web/20211011233005/https://developer.qualcomm.com/sites/default/files/docs/adreno-gpu/developer-guide/gpu/spec_sheets.html
- Qualcomm Adreno Vulkan Developer Guide 80-NB295-7 (not read; third-party mirror only): https://www.scribd.com/document/527887801/80-nb295-7-a-adreno-vulkan-developer-guide

Igalia / Turnip developers and others:
- [I1] Danylo Piliaiev, "Low-resolution-Z on Adreno GPUs": https://blogs.igalia.com/dpiliaiev/adreno-lrz/
- Samuel Iglesias, "Low Resolution Z Buffer support on Turnip": https://blogs.igalia.com/siglesias/2021/04/19/low-resolution-z-buffer-support-on-turnip/
- Faith Ekstrand, "Does subgroup/wave size matter?" (cited in ir3.c): https://gfxstrand.net/faith/blog/2020/10/does-subgroup-wave-size-matter/
- [C1] Chips and Cheese, "Inside the Snapdragon 855's iGPU" (A640 measurements: L1 1 KB, L2 128 KB, TLB latency): https://chipsandcheese.com/p/inside-the-snapdragon-855s-igpu
- [C2] Chips and Cheese, "Correction on Qualcomm iGPUs" (A640 double-rate FP16 via wave128; 2x64-wide FP32 + 2x128-wide FP16 ALUs): https://chipsandcheese.com/p/correction-on-qualcomm-igpus
- azhirnov, Adreno guide (secondary compilation): https://github.com/azhirnov/cpu-gpu-arch/blob/main/gpu/Adreno_Guide.md
- [W1] Wikipedia, Adreno (A630 row; unsourced figures): https://en.wikipedia.org/wiki/Adreno
- [S1] CNX Software, Snapdragon 845 specs ("LPDDR4x, 4×16 bit up to 1866MHz"): https://www.cnx-software.com/2017/12/07/qualcomm-snapdragon-845-octa-core-kryo-385-soc-to-power-premium-smartphones-xr-headsets-windows-laptops/

Not found: any Qualcomm GDC, Vulkanised or SIGGRAPH talk specific to a6xx shader occupancy or wave size. The architecture numbers
above come from Mesa (reverse-engineered), Linux and third-party measurements, not from Qualcomm.
