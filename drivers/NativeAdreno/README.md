# NativeAdreno

Native XNU Adreno 740 driver. Owns GMU startup, the CP ring, BOs, native 4 KiB SMMU contexts, fences and cache maintenance.

Current state: GPU commands and the scanout copy run on sheng hardware. Shader compilation is slow and there is no complete precompiled UI pack.

Linked into XNU by `unixnu build kernel`.
