# NativeUfs

Native UFS storage driver. Owns the host controller, SMMU, queues and the root read path.

Current state: the native UFS root works on sheng.

Linked into XNU by `unixnu build kernel`.

## fajita copy

This copy of [UniXNU/NativeUfs](https://github.com/UniXNU/NativeUfs) at `af785c1` overrides the base module for the OnePlus 6T. It changes the apps_smmu stream of the UFS host, which SDM845 maps with a mask (`iommus = <&apps_smmu 0x100 0xf>`) in a 0x80000 register window:

- `native-ufs-smmu-stream`, `native-ufs-smmu-mask` and `native-ufs-smmu-size` boot args; the defaults are sheng's values (0x60, 0, 0xd0000).
- The stream-match register carries the mask; an existing entry is reused only when it is exactly that value on the reserved bypass bank, and any other entry overlapping the stream's IDs is a conflict.
- `native-ufs-smmu-adopt=1` keeps the firmware's own entry for the stream when it is exactly that value on a bank with stage 1 translation off (fajita: slot 5 = 0x800f0100 on bank 2), without an SMMU write; off by default.
- `tests/SmmuTakeover.cpp` covers the SDM845 geometry, the masked stream and the adoption.

It also selects one of several root images in userdata:

- `native-ufs-root-base=<MiB>` is the byte offset (MiB * 1048576) in userdata of the root manifest; absent or 0 reads the first block, as before. A base at or past the partition end is rejected with manifest status 4.
- Manifest version 2 is version 1 plus a u64 `base_bytes` at byte 4056, inside the hashed bytes 0..4063. Window offsets and `image_bytes` are relative to the manifest. Version 1 is accepted only at base 0; version 2 only when `base_bytes` equals the requested base. Any other combination is manifest status 8 (base mismatch). `base + image_bytes` must fit in the partition (status 4 otherwise).
- Windows are published at `userdata + base + offset`. The Data store and the diagnostic log treat `base + image_bytes` as the end of the root image, so neither can open over the selected image.
- `tests/RootLayout.cpp` covers version 1 and 2 at base 0 and at a nonzero base, base mismatches and the bounds.
