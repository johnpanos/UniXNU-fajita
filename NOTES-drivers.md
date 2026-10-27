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
