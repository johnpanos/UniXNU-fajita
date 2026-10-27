#!/bin/bash
# scripts/fajita/boot.sh [PAYLOAD BOOTIMG]: boot a build on the phone, which must be in fastboot (bootloader) mode.
# Flashes the payload to system_a (unused: slot b is active), marks slot b active and bootable, and `fastboot boot`s the boot image
# (nothing is written to the boot partitions). Defaults: the output of scripts/fajita/build.sh.
# fastboot runs in its own session: a kill of this script's process group (bg.sh kill, --max) then never cuts a fastboot
# exchange in half, which wedged the bootloader twice (no command answered until a hardware reset). Bounded by a timeout.
fb() { setsid -w timeout ${FB_TIMEOUT:-120} fastboot "$@"; }
set -eo pipefail
BOARD=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${FAJITA_OUT:-$BOARD/.cache/fajita-out}
PAYLOAD=${1:-$OUT/payload.img}; BOOT=${2:-$OUT/boot.img}
[ -f "$PAYLOAD" ] && [ -f "$BOOT" ] || { echo "no build: run scripts/fajita/build.sh first"; exit 1; }
for i in $(seq 1 30); do [ -n "$(fb devices)" ] && break; [ $i = 1 ] && echo "waiting for the phone in fastboot ..."; sleep 2; done
[ -n "$(fb devices)" ] || { echo "no phone in fastboot"; exit 1; }
echo "battery: $(fb getvar battery-voltage 2>&1 | head -1 | grep -o '[0-9]*$') mV"
fb flash system_a "$PAYLOAD"
for i in 1 2 3 4 5; do
  fb set_active b >/dev/null 2>&1
  [ "$(fb getvar slot-unbootable:b 2>&1 | head -1)" = "slot-unbootable:b: no" ] && break; sleep 2
done
fb boot "$BOOT"
echo "booting; the USB debug channel (scripts/fajita-usbdbg-hub.py) comes up about 20 s in"
