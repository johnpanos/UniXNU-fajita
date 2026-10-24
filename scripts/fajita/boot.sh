#!/bin/bash
# scripts/fajita/boot.sh [PAYLOAD BOOTIMG]: boot a build on the phone, which must be in fastboot (bootloader) mode.
# Flashes the payload to system_a (unused: slot b is active), marks slot b active and bootable, and `fastboot boot`s the boot image
# (nothing is written to the boot partitions). Defaults: the output of scripts/fajita/build.sh.
set -eo pipefail
BOARD=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${FAJITA_OUT:-$BOARD/.cache/fajita-out}
PAYLOAD=${1:-$OUT/payload.img}; BOOT=${2:-$OUT/boot.img}
[ -f "$PAYLOAD" ] && [ -f "$BOOT" ] || { echo "no build: run scripts/fajita/build.sh first"; exit 1; }
for i in $(seq 1 30); do [ -n "$(fastboot devices)" ] && break; [ $i = 1 ] && echo "waiting for the phone in fastboot ..."; sleep 2; done
[ -n "$(fastboot devices)" ] || { echo "no phone in fastboot"; exit 1; }
echo "battery: $(fastboot getvar battery-voltage 2>&1 | head -1 | grep -o '[0-9]*$') mV"
fastboot flash system_a "$PAYLOAD"
for i in 1 2 3 4 5; do
  fastboot set_active b >/dev/null 2>&1
  [ "$(fastboot getvar slot-unbootable:b 2>&1 | head -1)" = "slot-unbootable:b: no" ] && break; sleep 2
done
fastboot boot "$BOOT"
echo "booting; the USB debug channel (scripts/fajita-usbdbg-hub.py) comes up about 20 s in"
