# fajita: PMICs over SPMI (PM8998 + PMI8998): charging, keys, battery

iOS 16.7.16 board. Logs of the runs are in the session scratchpad, `logs/pmic-*`.

| Item | State |
| --- | --- |
| SPMI bus (`drivers/NativeSpmi`, `libNativeSpmi.a`) | works on hardware: arbiter v5 (`0x50000000`), 262 APIDs, 208 peripherals mapped, 91 writable by EE 0 (pmic-s1) |
| Charging (`fajita-pmic=2`, board default) | works: the bootloader leaves charging off; after the pmOS charger setup the battery takes +1.0 A and its voltage rises (pmic-s2) |
| Keys (`fajita-pmic-keys=1`, `native-hid-buttons=1`) | real keys read correctly while released; the HID path is in test |
| Battery source (`fajita-pmic-batt=1`) | in test |
| bq27411 gauge (`fajita-pmic-bq=<n>`) | in test |

## Pieces

| File | What |
| --- | --- |
| `drivers/NativeSpmi/Spmi.[ch]` | arbiter v5 core: APID map, observer reads, owned-channel writes, write policy (host-tested) |
| `drivers/NativeSpmi/NativeSpmi.[ch]pp` | kernel binding and the C API `native_spmi_*` (see its README) |
| `package/FajitaPmicBoard.[ch]` | board constants, the charger sequence, key decoding, FG and bq27411 conversions (host-tested) |
| `package/FajitaPmic.cpp` | the bring-up thread: dump, charger, status rows, key poll, IOPMPowerSource, bq27411 |
| `package/FajitaGaugeI2c.[ch]` | the GENI I2C engine renamed `fpm_*`, so this archive can sit next to libFajitaTouch.a |
| `drivers/NativeHidInput` | `native-hid-buttons=1`: report 2 = Consumer Menu, Power, Volume Increment, Volume Decrement |
| `scripts/build-native-spmi.py`, `scripts/build-fajita-pmic.py` | the two archives; `scripts/check-fajita-pmic-host.py` the host tests |

## Boot arguments

| Argument | Effect |
| --- | --- |
| `fajita-pmic=1` | read only: APID table, PMIC identities (REVID), register dump of PON, GPIO 5/6, PMI8998 charger 0x10-0x16, FG 0x40-0x41, RRADC 0x45, then a status row every period |
| `fajita-pmic=2` | also the charger setup below (iOS 16 board default, in `board_args` with `fajita-pmic-dump=0 fajita-pmic-period=30`) |
| `fajita-pmic-fcc=<mA>`, `fajita-pmic-icl=<mA>` | fast charge (1000, at most the pmOS 1950) and USB input limit (1500, at most 1500) |
| `fajita-pmic-period=<s>`, `fajita-pmic-dump=0` | status period (10); skip the dump |
| `fajita-pmic-keys=1` | poll power (PON RT_STS bit 0), volume down (GPIO 5), volume up (GPIO 6) every 20 ms, read only |
| `fajita-pmic-keys-synth=<s>` (`-count`, `-mask`) | unattended test: synthetic volume up, volume down, power presses (200 ms, 3 s apart) from <s> s |
| `native-hid-buttons=1` | NativeHidInput sends the keys as consumer usages 0x30, 0xE9, 0xEA |
| `fajita-pmic-batt=1` | publish the battery as an IOPMPowerSource (class AppleSmartBattery) |
| `fajita-pmic-bq=1` / `=2` | the bq27411 on i2c10: read-only clock/pin snapshot / also enable SE2 and read the gauge |
| `native-spmi-ro=1` | the SPMI bus refuses every write |

## Charging

What the bootloader leaves (pmic-s1, PMI8998 SID 2): `CHARGING_ENABLE_CMD 0x1042 = 0`, `CHGR_CFG2 0x1051 = 0xc2` (enable by the
hardware pin), `FAST_CHARGE_CURRENT 0x1061 = 0`, input limit 500 mA (`0x1370 = 0x14`), USB online as SDP (APSD `01`), float
voltage `0x1070 = 0x73` (about 4.34 V).

`fajita-pmic=2` writes the pmOS `qcom_smbx` PMI8998 setup (`smb2_init_seq` with its masks and values, then its fast-charge and AICL
rerun writes), each read before and read back after, logged `chg ADDR old>new`:

| Register | Value | Why (pmOS) |
| --- | --- | --- |
| 0x1368, 0x1358, 0x1153 | UFP role / factory mode off / OTG by software | Type-C defaults |
| 0x107d, 0x1362 | recharge by VBAT, HVDCP off | |
| 0x1042, 0x1051 | charging enable by software command (CFG2 = 0x01) | the bootloader's pin control never enables |
| 0x1690, 0x1366 | STAT override, USB51 mode | |
| 0x1653, 0x1651 | charger watchdog off | |
| 0x1381, 0x1384, 0x1380 | AICL thresholds, AICL on | |
| 0x1060, 0x1061 | pre-charge 500 mA, fast charge 1.0 A | 1.0 A is the mainline value (pmOS 1.95 A) |
| 0x1661 | AICL rerun 3 s | |
| 0x1370 | input limit 1.5 A | charge.sh's `current_max` |

Never written: float voltage, JEITA, any PON register (also refused by the SPMI write policy).

Result (pmic-s2, phone on the host's USB port): vbat 3721 mV / -20 mA before; 3952 mV / +1013 mA two seconds after; AICL settles
at 1.5 A; +620..1040 mA for the rest of the run while iOS runs, 3999 mV after 4 min. The PMI8998 FG's voltage matches fastboot's
`battery-voltage` (3724 mV).

## Keys

PM8998 PON `RT_STS 0x810` bit 0 is KPDPWR (pressed = 1); GPIO 5 (volume down) and GPIO 6 (volume up) are inputs with pull-up, the
level in `RT_STS` bit 0 (pressed = 0). The bootloader leaves the GPIOs configured (`MODE_CTL 0x40 = 0`, pull 30 uA, enabled), so
nothing is written. Released state seen: `pon00 vd01 vu01`.

## Battery level

The PMI8998 fuel gauge's voltage and current are plain register reads (BATT_INFO 0x41a0, 0x41a2) and correct, but its monotonic
SoC (0x4009) is not a usable percentage on this phone: the FG node is disabled in mainline and pmOS (OnePlus uses the bq27411 on
i2c10 at 0x55), and in pmic-s2 it rose 18 -> 24 % for about 80 mAh (2 % of 3700 mAh). The percentage comes from the bq27411.
