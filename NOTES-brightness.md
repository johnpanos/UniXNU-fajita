# fajita brightness, alert slider and vibration (iOS 16 board)

All three are default off (boot arguments) and nothing else changes unless they are set.

## Brightness (`fajita-brightness=1|2`, package/FajitaPanel.cpp)

- Panel: Samsung S6E3FC2X01 (mainline panel-samsung-s6e3fc2x01.c), DCS 0x51 with a 2-byte big-endian value, DBV 0..1023, sent in
  high-speed mode (LPM cleared), 0x53 = 0x20 and 0x55 = 0 are set once by the bootloader.
- Sending: the SDM845 DSI0 command DMA (msm dsi_host.c msm_dsi_host_xfer_prepare / dsi_cmd_dma_add / xfer_commit). This DSI has one
  more register before CMD_DMA_CTRL than dsi.xml: from 0x38 up every offset is 4 higher than Linux's (live: CMD_DMA_CTRL 0x3c,
  CMD_CFG0 0x40, CMD_CFG1 0x44 = 0x00013c2c, DMA_BASE 0x48, DMA_LEN 0x4c, ACK_ERR 0x68, RDBK 0x6c, TRIG_CTRL 0x84, TRIG_DMA 0x90,
  INTR_CTRL 0x110). A layout guard refuses to write unless CMD_CFG1.INSERT_DCS and CMD_DMA_CTRL.FROM_FRAME_BUFFER are set.
- The MDSS SMMU context bank 0 is bypassed (SCTLR 0xe0), so DMA_BASE is physical: the buffer is the tail of the splash carve-out
  (0x9d400000 + 0x1800000).
- Proof: INTR CMD_DMA_DONE rises after the trigger (0x2 -> 0x503), ACK_ERR 0, and DCS 0x52 reads back what was written
  (`kcmd brightness read` -> `RDBK 22003c22` for DBV 60, `2203ff2b` for 1023).
- `kcmd brightness N | read | regs`.
- iOS: the iPhone X tree has /backlight (IONameMatch backlight -> AppleARMBacklight, AppleARMPlatform). `FAJITA_BACKLIGHT=1` makes
  make-fajita-adt.py add that node (`--backlight`; `--backlight-from` copies the calibration tables from the 20H392 DeviceTree at build
  time: `ipsw img4 im4p extract DeviceTree.d221ap.im4p`, nothing of it is stored here). The service never registers itself because its
  function-backlight_update target (phandle 0x80, the panel node, fourcc 'Uklb') is not resolvable, so FajitaPanel registers it, then
  follows `IODisplayParameters.rawBrightness` (0..2047) into the panel (dbv = raw * 1023 / 2047). CoreBrightness then finds the service
  and backboardd's slider/auto-dim land in rawBrightness.

## Alert slider (`fajita-alert=1|2`, package/FajitaAlertSlider.cpp)

TLMM GPIO 126 (top, Silent), 52 (middle, Vibrate), 24 (bottom, Ring), active low (sdm845-oneplus-common.dtsi alert-slider),
bias-disable applied. `kcmd alert`. `fajita-alert=2` reports it to iOS as ringer HID usage Telephony 0x2E (AppleM68Buttons maps
"ringerab" to usage page 0x0b usage 0x2e): `native-hid-buttons=1 native-hid-ringer=1`, `FAJITA_RINGER=1` for the /buttons `ringerab` name.
Ring = ringer on; Vibrate and Silent = silent. `fajita-alert-invert=1` flips the polarity (not yet verified on the phone).

## Vibration (`fajita-haptics=1|2`, package/FajitaHaptics.cpp)

PMI8998 haptics, SPMI SID 3, base 0xc000, register sequence of the Qualcomm SPMI haptics driver (Caleb Connolly, lkml v7:
LRA, buffer mode, sine, 400 mA, wave-play-rate 4255 us). `kcmd vibrate MS [VMAX_MV] | stop`. iOS hook (the Taptic path:
AppleHapticsSupport / hapticd / AppleAOPAudioHapticLEAP) is not done.
