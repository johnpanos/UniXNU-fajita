# iOS 16 for the iPhone X: the second fajita userland

fajita boots iPadOS 17.7.11 (iPad7,1, 21H461). iOS 16.7.16 for the iPhone X is a second boot option next to it: a second
kernel, ADT, trust cache and Data seed on the payload volume, a second root image in userdata, and a second entry in the
UniXNU menu. This note records the choice of route, the design, and the state of each work item.

## Why iOS 16

| Image | Device | Chip | Userland | fajita |
| --- | --- | --- | --- | --- |
| iPadOS 17.7.11 (21H461) | iPad7,1 | A10X, `0x8011` | `arm64` | runs; first entry |
| iOS 16.7.16 (20H392) | iPhone10,6 (D221AP) | A11, `0x8015` | `arm64` | second entry |
| iOS 17 and later | iPhone11,2 and newer | A12 and newer | `arm64e` | does not run |

SDM845 (Cortex-A75 and A55) is ARMv8.2 and has no pointer authentication. `arm64e` code uses PAC instructions outside the
hint space, such as `retab` and `braa`, which are undefined on ARMv8.2. iOS 17 dropped the iPhone 8 and iPhone X, so every
iOS 17 iPhone image is `arm64e`, and iOS 16 on the A11 is the newest iPhone userland fajita can run.

The image, checked on 2026-10-22 with `curl -s "https://api.ipsw.me/v4/device/iPhone10,6?type=ipsw"`: 16.7.16 (20H392) is
the newest 16.7.x build and still signed. One IPSW serves iPhone10,3 (D22AP) and iPhone10,6 (D221AP); the board uses the
D221AP identity (board ID 14). Its kernelcache (`kernelcache.release.iphone10b`) is
`Darwin Kernel Version 22.6.0 ... xnu-8796.142.1.703.8~1/RELEASE_ARM64_T8015`. Unlike the A10X image, 20H392 has
`Cryptex1,SystemOS` (2.74 GB, the dyld shared cache) and a 10 MiB `Cryptex1,AppOS`, so the windows are OS and SystemOS, as
for 21H461.

The iPhone X panel is 1125x2436; fajita is 1080x2340. The aspect ratios are close, so the iPhone layout fits without the iPad
width problem (at 2x the iPadOS image sees a 540-point-wide screen, narrower than any iPad).

## Route: UniXNU source kernel, not the ios-sweet kernel

Two routes have put iOS 16.7.16 userland on Qualcomm phones. The decision rests on what is in the repositories:

| | guacamole / ios-sweet route | UniXNU route (chosen) |
| --- | --- | --- |
| Kernel | `ios-sweet` xnu-8796.141.3 with 91 board patches, 16 KiB pages, monolithic: `SWEET HFS`, `SWEET AMFI`, `sweet_ufs`, framebuffer, touch and buttons are built in; no Apple kexts load | xnu-8796.141.3 from source with the UniXNU patches, 4 KiB pages; the 20H392 kernelcache's own kexts (APFS, AMFI, Sandbox, IOKit families) are prelinked |
| Kernel build | macOS GitHub runners only (`blacktop/darwin-xnu-build`, `xnu.yml`) | Linux, the fajita-linux toolchain already in use |
| Kernel in the repo | none: UniXNU-guacamole takes `inputs/guacamole/kernel.macho`, built elsewhere | built by `unixnu build kernel` |
| Root | HFS+ volume rebuilt from OS, SystemOS and AppOS (`tools/mkiosroot.sh`, `sweet-root-tweaks.py`: Setup skipped, activation seeded, daemons removed), loop-mounted with sudo and `hfsplus` | the unchanged APFS OS and SystemOS images behind a manifest, owner-verified block by block (NativeUfs, VerifiedRoot) |
| Security | permissive AMFI and sandbox, stub AppleKeyStore | original AMFI with trust caches |
| Loader | needs a DarwinBoot patch: kernel window congruent mod 32 MiB for the 16 KiB bootstrap blocks | current DarwinBoot |
| On fajita | edi194/ios-fajita (ios-sweet retargeted, 2026-10-20) hangs in `kernel_bootstrap_thread` near `kdp_init`, no root yet | the iPad kernel built the same way reaches the root mount on fajita; NativeUfs, SMMU adoption and the payload path are proven |

The prior art does not support mixing them: UniXNU-guacamole's NOTES (2026-10-22) record that `NativePrelink` cannot
prelink the 20H392 kernelcache onto the ios-sweet kernel (`_vfs_getvfs_with_vfsops` has 0 native address candidates, and
the kernel lacks `_kalloc_sparse_prelink_index_version` and `_native_prelink_security_init_version`). Its `kernel-8796`
workflow, an attempt at the UniXNU route, names a `guacamole_8796_defconfig` that is not in the repository and ignores the
build result (`|| true`).

The ios-sweet route reached the iOS home screen, but taking it would mean a second kernel tree with SM7150/SM8150 drivers to
port, a second root format and builder, and a second loader patch, and it would share nothing with the iPad entry except the
menu. The UniXNU route shares the payload, NativeUfs, VerifiedRoot, the trust cache path and the boot arguments, and its
cost is bounded and measured:

- 45 of the 57 XNU, NativePrelink and Linux-host patches apply unchanged to xnu-8796.141.3 (both NativePrelink patches
  among them). 12 need a rebase (list below).
- 15 of the 17 DriverKit `.iig` files of xnu-8796.141.3 are byte-identical to the sources of the recorded iig outputs
  (21H461). `IODispatchQueue.iig` and `OSObject.iig` differ only by additions outside the classes (an enum, `IIG_CONCRETE`
  and the `#ifndef KERNEL` ownership macros), so the 8796 build uses the 10063.121.3 copies and the same recorded outputs.
  No new macOS iig run is needed.
- All 31 prelink roots of the board exist in the 20H392 kernelcache (240 kexts).

ios-sweet stays the reference for the userland facts it established on hardware: the dev-fused security tuple that makes
mobileactivationd take its Hactivation branch (already used for the iPad), `home-button-type = 2`, and the iPhone `/product`
node copied from the real device tree.

## Design

### Two boards in one repository

`ios16/` is a second device directory: `ios16/board.toml` (`name = "fajita-ios16"`), `ios16/configs`, `ios16/feeds.toml`
(xnu-8796.141.3) and `ios16/unixnu.lock`, with `drivers`, `kits`, `package` and `scripts` linked to the top-level ones. The
unixnu driver keeps everything per board name, so the iOS 16 build has its own `out/fajita-ios16/` tree, feeds, iig outputs,
storage audit and verified-root caches, and never touches the iPad build. Run it with `-D ../UniXNU-fajita/ios16`.

### Payload: two menu entries

UniXNU reads one `\EFI\UniXNU\unixnu.toml` with any number of `[[entry]]` tables (docs/reference/boot-configuration.md).
DarwinBoot derives the sidecars from the entry's kernel path (`<path>.adt`, `<path>.trustcache`, `<path>.gpuaddon`) and takes
the entry's own `initrd` and `args`. So each entry carries its own ADT, trust caches and Data seed:

```
EFI/UniXNU/unixnu.toml                      two [[entry]] tables: "xnu" (iPadOS 17), "ios16" (iOS 16)
EFI/UniXNU/Payloads/kernel.macho(.adt, .trustcache), scratch-data.img          iPadOS 17.7.11
EFI/UniXNU/Payloads/ios16/kernel.macho(.adt, .trustcache), scratch-data.img    iOS 16.7.16
```

Every `unixnu build payload` now also leaves its entry under `out/<board>/payload/entry/` (the Payloads files and an
`entry.json` with the title, arguments and initrd). The iPad board lists `[[payload.include]] board = "fajita-ios16"`, and
its payload step copies that entry into `Payloads/ios16/` and writes the second `[[entry]]`. The iPad entry stays the
default.

### Userdata: a second root image after the first

```
userdata (LUN 0, 4096-byte LBAs 0x186f40..0x3ad1ffa, 229.2 GiB)
0         SHNGROOT v1 manifest, OS, SystemOS of 21H461      about 8.74 GiB, unchanged
16 GiB    SHNGROOT v2 manifest, OS, SystemOS of 20H392      about 7.0 GiB
...       free
end - 1 GiB - store   persistent Data store (sheng-data-persist, off on fajita)
end - 1 GiB           reserve: log export region and the 32 MiB log (sheng-log)
```

- The iOS 16 image starts at a fixed base of 16 GiB, which leaves the iPad image room to grow.
- Its manifest is version 2: version 1 plus a `u64 base_bytes` at byte 4056, inside the hashed range. Window offsets stay
  relative to the manifest, so the image is position independent and the builder is the same.
- The kernel argument `native-ufs-root-base=<MiB>` (iOS 16 entry: `16384`) makes NativeUfs read the manifest at that offset.
  A version 2 manifest is accepted only at the base it records, and a base other than 0 needs version 2, so a wrong argument
  finds no root rather than the other image. Without the argument NativeUfs reads offset 0, as before.
- The Data store and log guards use the end of the selected image (base plus image bytes).
- Root matching does not change: each ADT's `/chosen/root-matching` names its own OS volume UUID, and each kernel embeds
  only its own owner-signed policies, matched by window size.
- `unixnu build userdata` writes the iOS 16 image as an Android sparse image whose first 16 GiB are DONT_CARE chunks, so
  `fastboot flash userdata` writes only the iOS 16 region and leaves the iPad image and the end of the partition untouched.
  fastboot already sends the 8.74 GiB iPad image as sparse pieces, so the ABL handles DONT_CARE.
- When `sheng-data-persist` is turned on later, the two entries need different store places: give the iOS 16 entry a
  `sheng-log-reserve` that is the iPad reserve plus the iPad store size, so its store sits below the iPad one.

### Kernel

| Item | iPadOS 17 entry | iOS 16 entry |
| --- | --- | --- |
| XNU source | xnu-10063.141.1 (Darwin 23.6.0) | xnu-8796.141.3 (Darwin 22.6.0) |
| Patches | `patches/xnu` | `patches/xnu` with the replacements and skips of `patches/xnu-8796` |
| iig outputs | recorded 21H461 outputs | the same outputs; the 8796 tree takes the 10063.121.3 `IODispatchQueue.iig` and `OSObject.iig` |
| Kernelcache | 21H461, `f18b4df8...` | 20H392, `2ec297ba...` |
| Identity | iPad7,1, J120 | iPhone10,6, D221, `/product` of `DeviceTree.d221ap` |

## Work items

1. **XNU source.** UniXNU branch `ios16-stack`: a feed may name a patch series (`patch_series = "8796"`); the kernel script
   then applies `patches/xnu-8796/NAME.patch` instead of `patches/xnu/NAME.patch`, skips names listed in
   `patches/xnu-8796/skip`, and adds the series' own patches. `ios16/feeds.toml` pins xnu-8796.141.3
   (`1b191cb58250d0705d8a51287127505aa4bc0789`).
2. **Patches.** Rebase: `apple-only-cpu-tunables`, `external-archive-dependencies`, `native-aes-personality`,
   `offline-owner-systemos-mount`, `pgz-aligned-validation`, `public-entitlements-header`, `published-freezer-condition`,
   `source-machine-layer`, `zzx-pmap-sync-64bit-pa`, `zzy-native-gpu-addon-mount`, `zzy-staged-system-apps`,
   `zzz-boot-progress`.
3. **Drivers and kits.** Rebuild against the iOS 16 headers and compile records; check the corecrypto and libpthread archives
   against the 8796 kernel ABI.
4. **Prelink.** `kernelcache_sha256 = 2ec297ba...`, slide, `early_security_init` address of the 20H392 AMFI, roots.
5. **Identity.** `make-fajita-adt.py --iphone-identity`: model iPhone10,6, target-type D221, `/product` from the IPSW device
   tree, `home-button-type` 2, the same security tuple.
6. **Userdata and setup.** `[userdata] base_mib = 16384`, windows OS and SystemOS; the skip-setup seed needs the 20H392 build
   and version.
7. **Trust caches.** The static and System trust caches of 20H392, pinned in `ios16/board.toml`.

The state of each item is in the status table of `ios16/README.md`.
