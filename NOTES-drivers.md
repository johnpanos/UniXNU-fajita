# fajita drivers for iOS 16.7.16: inventory, ranking, and one section per driver

Goal (user, 2026-10-26): as many high quality drivers for the OnePlus 6T under iOS as possible, easiest first. Each driver sits behind
its own boot flag (default off) until it is proven on the phone with the iOS feature it enables working, then becomes default-on and
gets a section below. Branch `drivers` (worktree ../UniXNU-fajita-drivers), landed on ios16-port one driver at a time.

Out of scope: security and authentication hardware (fingerprint sensor, secure element, key storage), the modem's telephony side.

## Inventory (what the phone has, from sdm845-oneplus-common.dtsi / sdm845-oneplus-fajita.dts, against this tree)

| Hardware | Where | State in this port |
| --- | --- | --- |
| Panel S6E3FC2X01, DSI0/DPU | MDSS | driven (NativeLegacyDisplay, FajitaDpu, hw vsync) |
| Panel brightness (DCS 0x51) | DSI0 command DMA | built, **default off** (`fajita-brightness`, `FAJITA_BACKLIGHT`): the iOS slider path never proven |
| Adreno 630 | GPU | driven (FajitaAdreno, NativeAdreno, plugin) |
| Touch S3706B | GENI I2C12 | driven, default on |
| PMI8998 charger, PM8998 keys, bq27411 gauge | SPMI, I2C10 | driven, default on |
| PM8998 RTC | SPMI | driven, default on |
| UFS | UFS HC | driven |
| USB (DWC3 device) | usb_1 | driven (USB debug channel) |
| Wi-Fi WCN3990 | modem + SNOC | driven (Settings > Wi-Fi blocked, NOTES-wifi.md) |
| Alert slider | TLMM 126/52/24 | built, **default off** (`fajita-alert=2` ringer HID usage, `FAJITA_RINGER`): never proven in iOS |
| Vibration (PMI8998 LRA haptics) | SPMI SID 3 0xc000 | built, **default off** (`fajita-haptics`): kcmd only, no iOS hook |
| Thermal sensors TSENS0/1 (21 sensors) | 0x0c263000 / 0x0c265000 | read only by the GPU governor's caps; nothing for iOS, no CPU throttling |
| Hall sensor (magnet) | TLMM 124, active low | not driven |
| Flash LEDs (white + yellow, 1.5 A) | PMI8998 flash, SPMI | not driven |
| Accelerometer/gyro, magnetometer, light/proximity | behind the SLPI (sensor DSP, SSC); no APSS I2C nodes | not driven |
| Speaker amp TFA9894 | I2C4 0x34 | not driven (no mainline driver either) |
| Audio codec WCD9340, mics, earpiece | SLIMbus via ADSP (q6afe/q6asm) | not driven |
| Bluetooth WCN3990 | UART6, 3.2 Mbaud | not driven |
| Video codec (Venus) | venus | not driven |
| Cameras (IMX519/IMX376K, CAMSS) | CCI/CSI | not driven |
| GNSS | modem (QMI LOC) | not driven |
| NFC (NQ310) | I2C | out of scope (secure-element tied in iOS) |
| Fingerprint (Goodix in-display) | | out of scope |
| No notification LED, no headphone jack on the 6T | | |

## Ranking (easiest and most useful first)

1. **Brightness** to iOS (finish): the panel write and the AppleARMBacklight hook exist; proving the Settings/Control Center slider and
   making it default is a test and a flag flip.
2. **Alert slider** to iOS (finish): GPIO read and the ringer HID usage exist; prove iOS's silent switch follows it, then default.
3. **Thermal** (TSENS): MMIO reads already work (GPU governor); add CPU throttling from the same sensors and publish the temperatures
   the iOS way. Small, protects the phone.
4. **Hall sensor**: one GPIO; useful only as a cover lock signal, which iPhone iOS barely has (low value, low effort).
5. **Vibration** to iOS: hardware works; iOS 16 on the iPhone X plays haptics through the audio/Taptic stack (hapticd, CoreHaptics),
   so the iOS side is a real driver of its own (medium-hard).
6. **Flashlight**: PMI8998 flash LED is a few SPMI registers; iOS's flashlight goes through the camera stack (AVFlashlight ->
   mediaserverd -> the ISP driver), so the iOS side is hard without a camera driver.
7. **Motion and light sensors** (rotation, auto-brightness, proximity screen-off): highest user value, but every sensor sits behind
   the SLPI: PAS boot of slpi.mdt, GLINK/QRTR to it, the SSC QMI protobuf protocol, and FastRPC for the sensor registry (pmOS:
   hexagonrpcd + libssc). The Wi-Fi work's MDT/SCM/GLINK/QRTR/QMI pieces are reusable; still hard.
8. **Audio** (speaker, earpiece, mics): ADSP boot plus APR/q6afe, the WCD9340 over SLIMbus, the TFA9894 amp, and an iOS audio driver
   (IOAudio2/AppleAOPAudio class). Very hard.
9. **Bluetooth**: WCN3990 over UART with Qualcomm's HCI vendor setup; iOS's bluetoothd only knows Apple/Broadcom controllers. Very hard.
10. **GNSS**, **video decode** (Venus), **cameras**: each needs a large firmware protocol and an Apple-private kernel interface. Hardest.

## Worktree setup

`scripts/fajita/worktree-cache.sh` links a new worktree's `.cache/*` (all but `fajita-out*`) and `ios16/.cache` to the main tree's.

## 1. Brightness (default on since 0f78c13)

- What iOS does with it: Control Center's and Settings' brightness sliders, auto-dim, and CoreBrightness's ramps set the panel.
- Pieces: package/FajitaPanel.cpp (DCS 0x51 through the DSI0 command DMA, NOTES-brightness.md), the iPhone X /backlight node in the
  ADT (`--backlight` in ios16/board.toml, its calibration copied from the 20H392 D221 DeviceTree named by build.sh's
  FAJITA_BACKLIGHT_FROM), `fajita-brightness=1` in board_args.
- Why it never worked before: make-fajita-adt.py runs from the build's copy of the tree, which has no .cache, so the node went out
  without `backlight-marketing-table` and AppleARMBacklight's start failed ("Need maketing table for the new Architecture"). A
  missing source is now an error.
- The panel follows `IODisplayParameters.rawBrightness` (0..2047 -> DBV 0..1023) from a 30 ms thread (`fajita-brightness-poll=<ms>`);
  at 1 s the slider moved the panel in late jumps (19 updates over the two drags now).
- AppleARMBacklight never calls the lcd service's `bklU` platform function on this tree (0 calls); the property watch is the path.
- Test: `scripts/fajita/test/brightness-check.sh` on a held session (CC slider up, down, DBV read back over DCS 0x52). Proven on the
  default build (session-232643): DBV 184 -> 974 (up) -> 421 (down), PASS.

## 2. Alert slider -> iOS ringer switch (default on)

- Pieces: package/FajitaAlertSlider.cpp (`fajita-alert=2`, TLMM 126/52/24, polled once a second) -> NativeHidInput's Telephony 0x2E bit
  (`native-hid-ringer=1`; set while the ringer is ON, i.e. Ring; Vibrate and Silent are silent) -> backboardd's ringer state
  (BKSHIDServicesGetRingerState) -> SpringBoard (SBRingerControl: the "Silent Mode On/Off" banner, com.apple.springboard.ringerstate).
- SpringBoard acts on the switch only when MobileGestalt's `ringer-switch` is true. libMobileGestalt answers it from /buttons
  `function-button_ringerab` (hashed name Ah5yfSlY2yycIQb92HbmrA). ADT `--ringer` adds it with `ringerab`; no AppleM68Buttons runs
  here, so its phandle is null.
- The MobileGestalt cache: answers are cached in
  /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist.
  The cache stays valid for the whole OS build (libMobileGestalt +0x2bec checks only CacheVersion, the build, ReleaseType and a
  compiled-in CacheUUID), so a device-tree change never reaches a Data volume whose cache predates it.
  iOS's own rebuild path is the sentinel `com.apple.MobileGestalt.plist.rebuild` (`_MGWriteRebuildCacheSentinel`). Written by the
  helper's user (mobile; as root it silently does nothing), it makes MobileGestaltHelper drop the cache at the next boot, and the
  answers come from the device tree again.
  **One-time step after a device-tree change on an existing Data volume:** `session.sh run /usr/sbin/fjbundle-<n> --mg-rebuild
  --mobile`, then boot once more. A fresh Data volume builds the cache from the current tree and needs nothing. There is no
  legitimate automatic trigger (iOS rebuilds on restore and update only), so this is not part of the boot flow.
- Proof (session-074035, after the rebuild): `kcmd alert sim silent` -> "Silent Mode On" banner with the red bell, ringerstate 0;
  `sim ring` -> ringerstate 1; MobileGestalt `ringer-switch` true. The real switch position is read the same way (it sat at Ring).
- `kcmd alert` shows the position; `kcmd alert sim silent|vibrate|ring|off` stands in for the switch.

## 3. Thermal (default on: fajita-thermal=2)

- package/FajitaThermal.cpp: the 21 TSENS sensors (tsens0 13, tsens1 8, v2 TM_Sn_STATUS) once a second, `kcmd thermal`, a status
  row, a log line per 10 C band above 70 C.
- Governor (mode 2): each CPU cluster's OSM LUT index follows the hottest of its core and L3 sensors. It steps down one level at 90 C,
  two at 95 C, and to the floor at 105 C (silver 748 MHz, gold 825 MHz; set with `fajita-thermal-floor-silver/-gold`). It steps back
  up one level per 5 s at or below 85 C. Votes go through `fajita_cpufreq_thermal_limit` in FajitaCpuFreq.cpp. Before this the CPUs
  sat at the top level (1766/2649 MHz) whatever the temperature. The real gold cluster reaches about 71 C during boot.
- The policy is FajitaThermalPolicy.h, host-tested by `scripts/check-fajita-policy-host.py`.
- Proof (session-234614): `kcmd thermal sim gold|silver 1080 60` brought both clusters to their floors, and the per-core clock
  measurement read c0-3 748 MHz and c4-7 825 MHz. After the sim ended they climbed back one level per 5 s.
- iOS side (mode 3, experimental, default off): 28 temperature HID services under the names thermalmonitord's D22 plist matches
  (PMGR SOC Die Temp Sensor0-3 with Avg:/Max:, ACC Temp Sensor0-2, PMU tdev1-5, tcal, tjunc). They are NativeHidInput sensor-mode
  devices (Apple vendor 0xff00/5 around Sensor-page Environmental Temperature, created by `native_hid_sensor_create`). All 28
  publish. A simulated 110 C for 60 s left com.apple.system.thermalpressurelevel at 0, so thermalmonitord does not take them yet.
  IOHIDEventDriver's copyEvent has no temperature case, and thermalmonitord calls IOHIDServiceClientCopyEvent; that is the next thing
  to check.

## 4. Vibration to iOS (fajita-haptics=3, default off; built, blocked on MobileGestalt's cache)

State (e2f2e82, sessions 072207/073304):
- Built: the HAL plug-in package/fajita-audio (Actuator, Speaker, Hawking), the overlay over /Library/Audio/Plug-Ins/HAL, the kernel
  sink /dev/fajita-haptic (FajitaHaptics.cpp mode 3), /product/haptics closed-loop (ADT --haptics), and the policy host test
  (scripts/check-fajita-policy-host.py). The full build is rc 0.
- On the phone: mediaserverd loads the plug-in at every boot ("fajita-audio: plug-in loaded in pid 28"), and the HAL and VirtualAudio
  query all three devices (unanswered optional selectors: cust, clok, cstb, taps, accs, auto, cfsz, clk#, srnd). No device's I/O ever
  starts, and a long press with a context menu (a UIKit haptic on an iPhone) sends nothing to the Actuator.
- Blocker (the slider's too): iOS processes answer DeviceSupportsClosedLoopHaptics, like ringer-switch, from MobileGestalt's cache, and
  the cache still says false. `fjbundle --mg` as root (which reads the cache) answers false for both; `fjbundle --mg --mobile`
  (which computes them on the spot from the device tree) answers true for both.
  The cache stays valid for the whole OS build: libMobileGestalt's validity check (+0x2bec) compares only CacheVersion, the build in
  CacheExtra, ReleaseType, and the compiled-in CacheUUID 42F6FA57-...; nothing from the device tree. So a device-tree change never
  reaches it. iOS's own way to rebuild it is a sentinel (_MGWriteRebuildCacheSentinel; MobileGestaltHelper: "Rebuild MGCache on
  boot was requested"). `fjbundle --mg-rebuild` calls it, but as root it did not lead to a rebuild at the next boot: the cache's
  mtime did not change. The open question is why the helper's rebuild computes false while an in-process answer computes true.

### What it takes (assessment, written before the build)

- iOS 16 on the iPhone X plays every haptic (keyboard taps, UIKit and CoreHaptics feedback, notification and ringer vibration) as a
  one-channel waveform through the audio server. The chain is mediaserverd's CoreAudio HAL, then VirtualAudio.plugin
  (/Library/Audio/Plug-Ins/HAL), then a device whose UID is "Actuator", using the effects chain `d22_actuator_general` from
  /Library/Audio/Tunings/D22/Haptics. There is no kernel vibrator interface to implement. On a real iPhone the device comes from
  AppleAOPAudioPlugin.driver and the AOP.
- So the iOS side is an AudioServerPlugIn (a CFPlugIn bundle) that presents an "Actuator" output and hands its samples to the kernel.
  The kernel side, FajitaHaptics.cpp (PMI8998 LRA, already working from kcmd), turns them into motor on/off with an amplitude.
  HackiOS (redwood) reports that VirtualAudio builds no route unless the companion devices it expects also exist: a "Speaker" output
  and a "Hawking" input, both silent stand-ins here.
- Delivery: HAL plug-ins load only from /System/Library/Audio/Plug-Ins/HAL and /Library/Audio/Plug-Ins/HAL, both on the sealed
  system volume. The stack's boot-time add-on mount (patches/xnu-8796/zzy-native-gpu-addon-mount.patch, the same mechanism that puts
  NativeAdrenoMetal.bundle over /System/Library/Extensions) would need one more union view, over /Library/Audio/Plug-Ins/HAL, keeping
  VirtualAudio.plugin visible. That is a stack change: fork commit plus a gpu-plugin.ref bump.
- Size: a stack patch, a HAL plug-in of roughly 800 lines written for this board, a kernel sample sink for FajitaHaptics, and
  several phone runs. Medium-hard, not a quick finish.

## 5. Flashlight: what it takes (assessment)

- The PMI8998 flash LED is a few SPMI registers (two channels, torch up to 500 mA per the DT).
- iOS's Control Center flashlight shows only with MobileGestalt camera-flash. That answer comes from /product/camera `flash`, which
  is easy to add to the ADT.
- Turning it on goes AVFlashlight -> mediaserverd's FigFlashlight -> the camera driver (the ISP's kernel user client). With no camera
  driver there is nothing for FigFlashlight to talk to. HackiOS made it work by injecting a library into mediaserverd, which this port
  does not do. A non-injecting route needs a kernel service that answers FigFlashlight's ISP torch calls: reverse engineering of a
  private interface. Hard.

## 6. Wall clock after a hard reset (FajitaRtc fix, default on)

- A hard reset (long power press, 2026-10-27) restarted the PM8998 RTC counter (397354 -> 175). The record (drivers/FajitaRtc,
  userdata reserve) then gave only its floor: the wall time it was last set at (2026-10-22) plus the counter. Every boot started
  about 4 days behind (src2) until a network time fix, which is the same failure that put 1970 frames into Biome (NOTES-daemons.md).
- Fix (459437d): `fajita-rtc-floor=<build time>` from build.sh. The time is never served earlier than the payload's build (src3); the
  uptime is not added, because the generic timer runs on across warm resets. Mode 2 also writes the calendar's time as the record a
  minute after start and every hour, so a boot without network continues from the last known time and never runs backwards.
- Proof: no Wi-Fi (session-074810): src3, the calendar started 90 s behind the host, and the record was refreshed at +60 s (seq 3).
  Wi-Fi (session-075522): timed's fix ("time set ... record written", seq 4), then the phone's time() equal to the host's to the
  second, and the record refreshed from src1.
