#!/bin/bash
# install-default.sh [--build] | --remove : make the phone boot iOS by itself on power-on (no host `fastboot boot`).
#   (default)  install .cache/fajita-out-daily (payload.img + boot.img): payload to system_a, boot image to boot_b, slot b active.
#   --build    first build that image: a stay-up build (no --test, no autoreset) with Wi-Fi, iOS networking, wifid and SSH on.
#   --remove   restore boot_b from the pmOS backup (.cache/firmware/fajita/pmos-boot_b.img.gz of the board repo, or of
#              ../UniXNU-fajita), so a reset lands in fastboot again for unattended tests (cycle.sh, wifi-test.sh).
# The phone must be in fastboot. Each power-on boot spends one slot-b retry and nothing marks the boot successful, so after
# about 7 boots the bootloader stops with "Load Error"; running this script again (or `fastboot set_active b`) restores them.
# While installed, a reset boots iOS again instead of fastboot: test tools that wait for fastboot after a reset need --remove.
set -eo pipefail
BOARD=$(cd "$(dirname "$0")/../.." && pwd); OUT=$BOARD/.cache/fajita-out-daily
fb() { setsid -w timeout ${FB_TIMEOUT:-120} fastboot "$@"; }
LOCK=${XDG_RUNTIME_DIR:-/tmp}/fajita-phone.lock
if [ "$1" = --remove ]; then
  for b in "$BOARD/.cache/firmware/fajita/pmos-boot_b.img.gz" "$BOARD/../UniXNU-fajita/.cache/firmware/fajita/pmos-boot_b.img.gz"; do
    [ -f "$b" ] && break; done
  [ -f "$b" ] || { echo "no pmOS boot_b backup found"; exit 1; }
  T=$(mktemp "$BOARD/.cache/boot_b.XXXX"); trap 'rm -f "$T"' EXIT; gunzip -c "$b" > "$T"
  flock "$LOCK" bash -c "$(declare -f fb); fb flash boot_b '$T' && fb set_active b"
  echo "boot_b restored from $b; a reset now lands in fastboot"; exit 0
fi
if [ "$1" = --build ]; then
  mac=$(python3 -I "$BOARD/tools/wifi/persist-wlan-mac.py" 2>/dev/null || true)
  reserve=$(sed -n 's/^PHYS_RESERVE=\${WIFI_PHYS_RESERVE:-\([^}]*\)}.*/\1/p' "$BOARD/scripts/fajita/test/wifi-test.sh")
  ssid=$(awk -F= '/^SSID=/{print $2}' "$HOME/.config/unixnu/wifi-kittycorner" 2>/dev/null)
  args=(fajita-wifi=7 native-ufs-partread=1 "phys-reserve=$reserve" fajita-net=wifi fajita-wifi-iokit=1 fajita-wifid=1 fajita-ssh=1)
  [ -n "$ssid" ] && args+=("fajita-wifi-ssid=$ssid"); [ -n "$mac" ] && args+=("fajita-wifi-mac=$mac")
  FAJITA_OUT=$OUT "$BOARD/scripts/fajita/build.sh" "${args[@]}"
fi
[ -f "$OUT/payload.img" ] && [ -f "$OUT/boot.img" ] || { echo "no image in $OUT (run with --build)"; exit 1; }
flock "$LOCK" bash -c "$(declare -f fb); fb flash system_a '$OUT/payload.img' && fb flash boot_b '$OUT/boot.img' && fb set_active b && fb reboot"
echo "installed: the phone now boots iOS on power-on (slot b, retries reset)"
