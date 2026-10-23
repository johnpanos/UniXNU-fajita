# NativeSpmi

The SPMI bus of SDM845 for the kernel: byte reads and writes of the PM8998 and PMI8998 registers through the Qualcomm PMIC
arbiter, version 5, as the apps processor (execution environment 0).

| File | What |
| --- | --- |
| `Spmi.h`, `Spmi.c` | the arbiter core: APID map, observer-channel reads, owned-channel writes, the board write policy. Pure register logic over caller-supplied accessors (no mapping, lock or allocation) |
| `NativeSpmi.h`, `NativeSpmi.cpp` | the kernel binding and the C API (`native_spmi_*`): maps the windows with `ml_io_map`, one spin lock with interrupts off per transaction |
| `tests/spmi-test.cpp` | host test against a register model of the arbiter and two PMICs (`scripts/check-fajita-pmic-host.py`) |

Built into `build/native-spmi/libNativeSpmi.a` by `scripts/build-native-spmi.py` and force-loaded through
`CONFIG_XNU_EXTRA_LD_ARCHIVES` (iOS 16 board defconfigs). Nothing touches the arbiter until the first call.

## API

```c
#include "NativeSpmi.h"
int native_spmi_init(void);                                                   // first call from a thread: maps, reads the APID map
int native_spmi_read(unsigned sid, unsigned addr, void *buf, unsigned len);  // 1..8 bytes inside one peripheral
int native_spmi_write(unsigned sid, unsigned addr, const void *buf, unsigned len);
int native_spmi_read8(unsigned sid, unsigned addr);                           // byte or negative errno
int native_spmi_write8(unsigned sid, unsigned addr, unsigned value);
int native_spmi_masked_write(unsigned sid, unsigned addr, unsigned mask, unsigned value);
int native_spmi_channel(unsigned sid, unsigned addr, unsigned *owner_ee);    // APID and its owner EE
```

Results are 0 or a negative XNU errno (`-EINVAL`, `-ENODEV` unmapped peripheral, `-EPERM` not this EE's channel / refused by the
policy / `native-spmi-ro=1`, `-ETIMEDOUT`, `-EIO`, `-ENOTSUP`). After the first call they may be used in any context, also with
interrupts disabled; each transaction waits at most 1 ms. `native_spmi_stats[]` and `native_spmi_row` hold the counters.

Slave IDs: 0 and 1 PM8998 (0: PON `0x800`, RTC `0x6000`/`0x6100`, GPIO n at `0xc000 + 0x100 * (n - 1)`), 2 and 3 PMI8998
(2: charger `0x1000`-`0x16ff`, fuel gauge `0x4000`-`0x44ff`, RRADC `0x4500`).

## Write policy

Refused whatever the caller: every PON peripheral (`0x08xx` on any SID: PS_HOLD, S1/S2/S3 reset and shutdown control), and on the
PMI8998 the charger's float voltage (`0x1070`) and JEITA configuration (`0x1090`-`0x109f`). Writes to a peripheral whose channel
another EE owns are never issued. `native-spmi-ro=1` refuses every write.

## Hardware

`sdm845.dtsi` `spmi@c440000`: core `0x0c440000` (size `0x1100`), chnls `0x0c600000`, obsrvr `0x0e600000`, intr `0x0e700000`,
cnfg `0x0c40a000`, `qcom,ee = <0>`. Register layout from the study of Linux `drivers/spmi/spmi-pmic-arb.c` (v6.16, `pmic_arb_v5`):

- core: VERSION `0x0` (v5 is `0x5000_0000` up to v7), FEATURES `0x4` (bits 10:0 APID count), APID map `0x900 + 4n`
  (bits 19:8 PPID = SID << 8 | peripheral, bit 24 IRQ owner).
- cnfg: ownership `0x700 + 4n` (bits 2:0 owner EE). A PPID can be mapped by several APIDs, one per EE that may write it; the one
  EE 0 owns is used, otherwise the first.
- Read channel `obsrvr + 0x10000 * ee + 0x80 * apid`, write channel `chnls + 0x10000 * apid`: CMD `0x0`, STATUS `0x8`
  (done, failure, denied, dropped), WDATA0/1 `0x10`/`0x14`, RDATA0/1 `0x18`/`0x1c`.
- Command: `opcode << 27 | (addr & 0xff) << 4 | (bytes - 1)`, EXT_READL 1, EXT_WRITEL 0.

Interrupts (`intr`, GIC SPI 481) are not used; consumers poll.
