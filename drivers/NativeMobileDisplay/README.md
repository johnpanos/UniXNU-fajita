# NativeMobileDisplay

Mobile display driver and GPU scanout hand-off. Validates swaps, retires frames and drives the firmware framebuffer.

Current state: the CPU and GPU scanout paths work. The GOP scanout has no vblank interrupt; with `sheng-vsync=<Hz>` a work-loop timer sends the original family's vsync message to the compositor.

Linked into XNU by `unixnu build kernel`.

## fajita copy

This copy of [UniXNU/NativeMobileDisplay](https://github.com/UniXNU/NativeMobileDisplay) at `c031d88` overrides the base module for the
OnePlus 6T. The only change is a logical display size for NativeLegacyDisplay (`NativeLegacyDisplay.cpp`, `NativeLegacyScale.h`,
`FajitaDisplayLogical.h`):

- `fajita-display-logical=<W>x<H>` (off by default): the original family's dimension fields hold W x H, so every size userland sees
  (IOMFB properties, modes, swap and surface validation) is the logical one, while the scanout stays the UEFI GOP panel (1080x2340).
  iOS needs a screen size MobileGestalt knows for the device identity: iPad7,1 is 2048x2732 at @2x, so fajita passes
  `fajita-display-logical=2048x2732` with `offline-legacy-unit-scale=2` (`/chosen/display-scale` 2, 1024x1366 points).
- Each frame is fitted onto the panel (2048x2732 -> 1080x1441 at y=449), centered, with a black letterbox written once. Every image pixel
  is a 2x2 box average (2 taps per axis, precomputed per column and row) of the source; `sheng-fb-diff=1` stores only the 64-byte
  blocks that changed against a shadow of the scaled image. The frame statistics still describe the logical source. The GPU hand-off
  (`sheng-fb-gpu=1`) is not used for a logical display.
- `fajita-display-ppi=<N>` (off by default): answers the dot pitch query with N when the provider has no `dot-pitch` (iPad7,1: 264).
- `FajitaDisplayLogical.h` exports the logical size and the image rectangle on the panel (`fajita_display_logical`) with the
  panel-to-logical transform for touch.

## Original class per kernelcache

`NativeLegacyDisplay` subclasses the original `IOMobileFramebufferLegacy` of the kernelcache the kernel is prelinked from.
`scripts/build-native-legacy-display.py` generates its class header (`OriginalLegacyFramebufferABI.h`) and the private layout
(`OriginalLegacyLayout.h`, also read by `package/LegacyDisplayProbe.cpp`) for 21H461 (iPad board) or 20H392 (ios16 board), from its
`LEGACY` table. What differs in 20H392 (iOS 16.7.16, iPhone10,6):

- instance size 0x1660 (21H461 0x18a0), 338 vtable slots (358); the generic IOMobileFramebuffer has 252 slots (262), so every
  Legacy slot from 212 on moved: supportedFrameInfo 212 (213), getDisplayDimensions 246 (249), startHardware 274 (288),
  kickSwapQueue 276 (290), completeCurrentSwap 281 (295), validateSwap 282 (296), getRefreshAttributes 287 (301),
  collectCompletionReports 321 (337), the state setter 336 (354); the guarded abstract leaves are 211, 242, 243, 292, 293, 322 and
  329 (211, 245, 246, 306, 307, 338, 345). isMainDisplay 184 and getDotPitch 188 are unchanged.
- supportedFrameInfo has no mask output: `(index, &count, key)`, selector 74 with one scalar and the 64-byte key.
- display fields: current swap +4872, pending head/tail +4880/+4888, vsync clients +4992, width/height +5208/+5212, generations
  +5220/+5224, state words +5236/+5240, retired frames +172 (21H461 +5472, +5480/+5488, +5592, +5808/+5812, +5820/+5824,
  +5836/+5840, +252). Swap request: surfaces +2184, queue links +2408/+2416 (+2624, +2960/+2968); the layer masks, options,
  destination size and rectangles keep their 21H461 offsets.
- IOSurface returns width, height, stride and allocation as 32-bit words (`ldr w0`, zero-extended into the 64-bit return).
