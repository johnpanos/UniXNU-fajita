# Bi-directional USB debug channel for fajita (iOS 16 userland)

Goal: a host-side command channel into the running iOS 16.7.16 userland on the
OnePlus 6T, to speed up touch-driver iteration. The phone runs postmarketOS mainline
Linux 6.16.7 in its other slot, where USB device mode works; mainline is the register
reference (clone in `/home/john/src/ref/linux`, v6.16, sparse).

## Route chosen, and why

The DWC3 CDC gadget is the gating item for every userspace route (ACM-to-agent,
ECM+SSH, or KDP). Bring it up once, then pick the userspace layer on top.

- **Fastest to a userspace shell:** CDC-ECM + an `ifnet` + dropbear. The userland is
  already built and signed on macOS CI and staged in
  `/home/john/src/UniXNU-fajita/.cache/ssh-userland` (dropbearmulti, the launchd plist
  `org.blu-sharky.sshd.plist`, `owner-ssh.bundle` trust cache, `ssh-addon.bin`). The
  iOS 16 AMFI stand-in treats ad-hoc-signed binaries as platform, and the GPU-addon
  union mount over `/System/Library/LaunchDaemons` (boot arg `sheng-gpu-addon=1`,
  already on in the board default args) starts the daemon as root.
- **Fastest to *any* host channel:** the CDC-ACM gadget carrying the kernel-side fjdbg
  protocol (already written and host-tested: `drivers/NativeDwc3/UsbDbgProto.h`,
  `UsbDbgDevice.h`, `scripts/fajita-usbdbg.py`). This reaches the kernel, not userland,
  but needs no signing/trust/userland work and is the natural first data path.

Plan: bring the gadget up as CDC-ACM first (host sees `/dev/ttyACM*` via the in-box
`cdc_acm` driver, no host driver work), prove bulk data with fjdbg, then add the ECM
function + `ifnet` for SSH into userland.

## Hardware facts (mainline DT + drivers, verified against sdm845.dtsi)

- dwc3 core `0x0a600000` size `0xcd00` (`snps,dwc3`); wrapper/QSCRATCH `0x0a6f8800`
  size `0x400`; QUSB2 v2 HS PHY `0x088e2000` size `0x400`. Board is USB2-only
  (`dr_mode = "peripheral"`, `maximum-speed = "high-speed"`, one HS PHY,
  `qcom,select-utmi-as-pipe-clk`).
- GCC `0x00100000`: `USB30_PRIM_BCR` reset `0xf000`, `USB30_PRIM_GDSC` `0xf004`,
  master CBCR `0xf00c`, sleep `0xf010`, mock-utmi `0xf014`, cfg-noc AXI `0x502c`,
  aggre AXI `0x8201c`, AHB2PHY CBCR `0x6a004`, AHB2PHY BCR `0x6a000`, QUSB2 PHY BCR
  `0x12000`. (`drivers/clk/qcom/gcc-sdm845.c`.)
- `apps_smmu` `0x15000000` size `0x80000`; dwc3 master stream `<&apps_smmu 0x740 0>`,
  `qcom,smmu-s1-bypass`. The NativeUfs planner (`SmmuTakeover.h`) already handles this
  stream (stream 0x740, mask 0) for the gadget's event buffer + TRB DMA.
- dwc3 IRQ GIC SPI 133; QUSB2 HS PHY status `PLL_COMMON_STATUS_ONE` at `0x1a0`,
  bit0 `CORE_READY_STATUS`.

## DWC3 bring-up order (mainline, SDM845)

Glue `dwc3-qcom.c` (`dwc3_qcom_probe`): assert+deassert `USB30_PRIM_BCR`; enable the
GCC USB30_PRIM clocks; `dwc3_qcom_select_utmi_clk` on QSCRATCH_GENERAL_CFG `0x08`
(set PIPE_UTMI_CLK_DIS, then PIPE_UTMI_CLK_SEL|PIPE3_PHYSTATUS_SW, then clear
PIPE_UTMI_CLK_DIS, ~100us each); `dwc3_qcom_vbus_override_enable` on QSCRATCH
HS_PHY_CTRL `0x10` (set UTMI_OTG_VBUS_VALID bit20 | SW_SESSVLD_SEL bit28) and
SS_PHY_CTRL `0x30` (LANE0_PWR_PRESENT bit24).

Core `core.c` (`dwc3_core_init`): GUID write; `dwc3_phy_setup` → GUSB2PHYCFG `0xc200`
clear SUSPHY(bit6), set/clear ENBLSLPM per quirk; QUSB2 PHY init (below); core soft
reset (DCTL `0xc704` bit30 CSFTRST + clear RUN_STOP, poll clear); global control
GCTL `0xc110` (clear SCALEDOWN, clk-gating per PWROPT); set PRTCAP=device (GCTL bits
[13:12]=2); event buffers (GEVNTADRLO/HI `0xc400/4`, GEVNTSIZ `0xc408`, clear
GEVNTCOUNT `0xc40c`). Then gadget start.

QUSB2 PHY init (`phy-qcom-qusb2.c`, `qusb2_v2` cfg): assert+deassert PHY reset (GCC
QUSB2PHY_PRIM_BCR `0x12000`, 100us in reset); power down (PORT_POWERDOWN `0x210` set
disable_ctrl = VREF_SUPPLY_TRIM|CLAMP_N_EN|POWER_DOWN); write the init table
(ANALOG_CONTROLS_TWO `0x04`=0x03, CLOCK_INVERTERS `0x18c`=0x7c, CMODE `0x2c`=0x80,
LOCK_DELAY `0x184`=0x0a, DIGITAL_TIMERS_TWO `0xb4`=0x19, BIAS_CONTROL_1 `0x194`=0x40,
BIAS_CONTROL_2 `0x198`=0x20, PWR_CTRL2 `0x214`=0x21, IMP_CTRL1 `0x220`=0x0,
IMP_CTRL2 `0x224`=0x58, PORT_TUNE1 `0x240`=0x30, TUNE2 `0x244`=0x29, TUNE3 `0x248`=0xca,
TUNE4 `0x24c`=0x04, TUNE5 `0x250`=0x03, CHG_CTRL2 `0x23c`=0x0, byte writes for the
8-bit TUNE regs); clear POWER_DOWN; wait 150us; poll PLL_STATUS `0x1a0` bit0 CORE_READY.
(The board tuning overrides imp-res-offset=8, hstx-trim and preemphasis are refinements,
not required for a link.) Mu-Silicium's `qcom,qusb-phy-init-seq` in the downstream DTS
matches these addresses/values.

Gadget start (`gadget.c`, `__dwc3_gadget_start`): GRXTHRCFG `0xc10c` clear PKTCNTSEL;
DCFG `0xc700` set NUMP + IGNSTRMPP(bit23) + speed=HS; DEPSTARTCFG (DEPCMD on ep0,
`0xc80c`); ep0-OUT/ep0-IN DEPCFG (SETEPCONFIG) + SETTRANSFRESOURCE; DALEPENA `0xc720`
bits 0,1; ep0 SETUP TRB (CONTROL_SETUP, 8 bytes) via STARTTRANSFER; DEVTEN `0xc708`
(CMDCMPLT|CONNECTDONE|USBRST|DISCONN|WKUP|...); DCTL RUN_STOP; then poll GEVNTCOUNT and
service USB reset → set DCFG addr 0, CONNECT_DONE → read DSTS speed, ep0 SETUP → the
standard + CDC requests (descriptors in the forthcoming `Dwc3Gadget.h`).

Polled, no interrupt: a kernel thread (like NativeUfs's) drains the event buffer; the
whole gadget is lock-free so it can also run from the panic/debug path.

## SMMU / DMA

Event buffer + per-endpoint TRB rings are the DMA masters behind stream `0x740`.
Allocate with `IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, …)` as NativeUfs does, then
`configureUfsSmmu` the 0x740 mapping (adopt the firmware bypass bank or plan a free
one). Cache maintenance `dc civac` + device-aligned stores, exactly as NativeUfs.

## Test plan (each step has the `fajita-wdog=3` + `wdstop` hardware backstop)

1. **probe (read-only)** `native-dwc3-probe=2`: the `U` row (BootLog) reports GCC
   clocks, GUSB2PHYCFG, QUSB2 PLL-ready, GCTL mode and the SMMU plan — tells us
   whether UEFI left the controller up or we bring the PHY/clocks up. Safe.
2. **core reset** (no run/stop): soft reset returns, event count sane. Nothing on host.
3. **enumeration**: ep0 + descriptors + run/stop. Host `lsusb`/`dmesg` shows the device.
4. **data**: bulk pair + `usb-debug=1`; `fajita-usbdbg.py hello/state/console` round-trip.
5. **userspace**: CDC-ECM + `ifnet`, then dropbear → `ssh root@10.0.0.2`.

Host side: `cdc_acm`/`cdc_ether` are in-box; a udev rule tags our VID/PID `uaccess`
for the ttyACM (the pmOS gadget already gets this; a rule for `1209:0001` /
`18d1:ecm` will be added under the user's udev dir if needed).

## Build / run

- `scripts/fajita/build.sh --test native-dwc3-probe=2`, then `scripts/fajita/test/cycle.sh usb-probe` (queues behind
  phone.lock). The `U` row comes back in `.cache/fajita-work/logs/usb-probe/`.

## Status (2026-10-23): working on hardware

Route taken: CDC-ACM gadget (kernel, polled) + framed fjdbg protocol + a userland agent.
SSH was not needed: iOS 16 has no shell, so the agent runs the binaries iOS ships
(`ioreg`, `ps`, `df`, `hidutil`, `nvram`, ...) and streams their output back.

```
host /dev/ttyACM0 (cdc_acm, 1209:4d69, seat ACL via systemd hwdb, no root)
  <-> fjdbg frames: LOG (kernel msgbuf, live) | CMD/RSP (kernel) | SHELL
  <-> NativeDwc3 gadget (DWC3 device mode, poll thread MAXPRI_KERNEL on CPU 1)
  SHELL <-> /dev/fjdbg <-> fjdbg-agent (= /usr/sbin/wifid via union mount, launchd root job)
```

Hardware milestones (logs under scratchpad/logs/usb-*):
1. probe: UEFI leaves the USB30 GDSC on, the QUSB2 PLL locked and the core in device
   mode (`U ... cap=2 phy=1 gdsc=f8222000`); the gadget reuses them (`clk=0 phy=0`).
2. enumeration: host `lsusb` `1209:4d69 Generic fajita usb-debug`, `/dev/ttyACM0`, about
   17 s after boot.
3. data: the kernel log streams to the host; host requests reach the kernel (bulk OUT).
4. userspace: `ioreg -c IOService -d 2` typed on the host runs in iOS userland and its
   output comes back (usb-enum9).

Bugs found on hardware, each from a BootLog `V` row:
- SMMU: the NativeUfs planner claims banks top-down and collided with UFS's bank;
  a dwc3-specific free-bank scan installs the VA64 bypass for stream 0x740.
- DEPSTARTCFG(2) belongs on physical ep 0 (dwc3_gadget_start_config).
- Device events: the type is in bits [11:8]; with it fixed, the speed (and so the
  512-byte HS bulk descriptors) is right.
- SET_ADDRESS: write DCFG.DEVADDR while handling the SETUP (dwc3_ep0_set_address).
- Starvation: ep0 has tight host timeouts; the poll thread runs at MAXPRI_KERNEL bound
  to CPU 1 (Linux services ep0 from the interrupt).
- The boot addon never reaches the iOS 16 kernel (BootLog `A files=-1` on every run, the
  touch board's too), so the kernel embeds the CI-signed agent and writes it itself.
- Kernel replies carried the device's running seq; the host matches on the request's.
- ep0 status stage: it must start on XferNotReady(control-status), on the endpoint the
  event names (dwc3_ep0_xfernotready). Starting it right after the SETUP/data stage
  wedged ep0 now and then (usb-enum8, 11, 12): later tty opens then blocked on cdc_acm's
  SET_LINE_CODING/SET_CONTROL_LINE_STATE and the shell frames never went out. ep0 OUT
  data TRBs are also rounded up to the max packet (the 7-byte SET_LINE_CODING).
- The log may fill only half the TX ring, so replies and agent output are never starved.

## How to use

Build and run (the debug channel is part of every build):
```
scripts/fajita/build.sh --test
scripts/fajita/test/cycle.sh NAME          # or session.sh start, for many commands in one boot
```
`usb-debug=1` stops after the core reset, `=2` enumerates and runs the kernel channel,
`=3` also installs and mounts the userland agent.

Host (no root; Python stdlib only):
```
T=scripts/fajita-usbdbg.py
python3 $T console                 # kernel log, live
python3 $T hello | state           # kernel command layer
python3 $T shell                   # interactive: ps -A | ioreg -l | hidutil list | @read /path
python3 $T run ioreg -c IOService -d 2   # one command: its output, exit status = the command's
```
`run` is the scripting interface (touch-driver iteration: `run ioreg -r -c FajitaTouch -l`,
`run hidutil list`, `run @read /some/file`); a cold first command can take several seconds.
Only one process can own the tty at a time (`cycle.sh CYCLE_USBLOG=1` is one such reader).
The agent: `package/usb-debug-tools/fjdbg-agent.c`, built and 4 KiB ad-hoc signed by
`.github/workflows/usb-debug-agent.yml`; download with
`gh run download <id> -n fjdbg-agent -D /home/john/src/UniXNU-fajita/.cache/usbdbg-agent`.

## Left

- Interrupt-driven ep0 (GIC SPI 133) instead of the 1 ms poll.
- Kernel memory/MMIO read hooks for the command layer (the protocol has them).
- KDP over the KDP channel (`kdp-proxy` on the host is written).
- Why the boot addon never arrives on the iOS 16 board (`A files=-1`).

## Phone-side note

The pmOS log reader's reboot helper (`/tmp/rb`) can hang in D state in the pmOS kernel's
shutdown path (seen after usb-enum12: nine `/tmp/rb` stuck, phone left in pmOS).
`echo b > /proc/sysrq-trigger` over the telnet shell resets at once and lands in fastboot;
`cycle.sh` could fall back to it.

## Screenshots over USB (no pmOS)

Op `shot` (u32 scale: 2 = half resolution, the default; 1 = full 1080x2340) replies with the frame
geometry, then the gadget streams the console scanout (PE_current_console, mapped like the diag
export) as BGR on the FILE channel; the host writes an RGB PNG (stdlib zlib). Half resolution
(540x1170, 1.9 MB) takes about 1 s; it matches the diag export's screen.png pixel for pixel in layout
and colour (usb-shot1 vs pmic-k2).

- one-off, tty free: `scripts/fajita-usbdbg.py screenshot OUT.png [--scale 1]`
- during a cycle (the hub owns the tty): `scripts/fajita-usbdbg-hub.py call screenshot OUT.png [--scale 1]`
- in a run: `CYCLE_SHOTS=45,100 cycle.sh NAME PAYLOAD BOOT` writes logs/NAME/shot-45.png, shot-100.png
  (seconds after "XNU booting"; results in shots.txt). `CYCLE_HUB_SCRIPT=` selects the hub script.
