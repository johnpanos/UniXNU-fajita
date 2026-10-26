#!/bin/bash
# user-boot.sh [--no-build] [BOOTARG...]: boot a build for a person to use by hand, and record its console the whole time.
# Builds a stay-up image (no --test: no autoreset, no watchdog stop) with Wi-Fi on and joined to the SSID in
# ~/.config/unixnu/wifi-kittycorner (the passphrase is the one already saved on the phone, /private/var/ngpu/wifi.psk), iOS's own
# network stack (fajita-wifi-iokit=1) and Apple's wifid (fajita-wifid=1), into .cache/fajita-out-user. Boots it under the phone lock,
# then streams the USB console into .cache/fajita-work/logs/user-<time>/usb-console.txt through bg.sh (job user-console) until the
# phone goes away, so a freeze leaves a log without a pmOS pull. Extra arguments are added boot arguments.
#   scripts/fajita/test/user-boot.sh                 # build, boot, record
#   scripts/fajita/test/user-boot.sh --no-build      # boot the last .cache/fajita-out-user image again
set -eo pipefail
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd); W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}
OUT=$ROOT/.cache/fajita-out-user; CREDS=${WIFI_CREDS:-$HOME/.config/unixnu/wifi-kittycorner}
build=1; [ "$1" = --no-build ] && { build=0; shift; }
if [ $build = 1 ]; then
  ssid=$(awk -F= '/^SSID=/{print $2}' "$CREDS" 2>/dev/null)
  mac=$(python3 -I "$ROOT/tools/wifi/persist-wlan-mac.py" 2>/dev/null || true)
  reserve=$(sed -n 's/^PHYS_RESERVE=\${WIFI_PHYS_RESERVE:-\([^}]*\)}.*/\1/p' "$TD/wifi-test.sh")
  args=(fajita-wifi=7 native-ufs-partread=1 "phys-reserve=$reserve" fajita-net=wifi fajita-wifi-iokit=1 fajita-wifid=1)
  [ -n "$ssid" ] && args+=("fajita-wifi-ssid=$ssid"); [ -n "$mac" ] && args+=("fajita-wifi-mac=$mac")
  FAJITA_OUT=$OUT "$ROOT/scripts/fajita/build.sh" --skip-plugin "${args[@]}" "$@"
fi
[ -f "$OUT/payload.img" ] || { echo "no image in $OUT (run without --no-build)"; exit 1; }
flock "${XDG_RUNTIME_DIR:-/tmp}/fajita-phone.lock" "$ROOT/scripts/fajita/boot.sh" "$OUT/payload.img" "$OUT/boot.img"
for i in $(seq 1 60); do lsusb -d 1209:4d69 >/dev/null 2>&1 && break; sleep 2; done
tty=$(ls /dev/ttyACM* 2>/dev/null | head -1); [ -n "$tty" ] || { echo "the debug gadget did not come up"; exit 1; }
L=$W/logs/user-$(date +%H%M%S); mkdir -p "$L"
"$TD/bg.sh" start user-console --max 86400 --quiet python3 -I "$ROOT/scripts/fajita-usbdbg.py" --port "$tty" \
  --log-file "$L/usb-console.txt" console
echo "booted; console -> $L/usb-console.txt (bg.sh kill user-console when done)"
