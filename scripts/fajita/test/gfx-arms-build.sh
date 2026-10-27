#!/bin/bash
# gfx-arms-build.sh ARM...: build the gfx A/B arms as Wi-Fi --test images (the web scenes need the network), each into its own
# .cache/fajita-out-gfx-ARM, so every arm boots with `WIFI_NO_BUILD=1 FAJITA_OUT=<that dir> wifi-test.sh ...` and nothing is rebuilt
# between the runs. The kernel and payload are this tree's; the plugin is copied (--skip-plugin) from PLUGIN_BASE (default
# .cache/fajita-out, build.sh's last plugin of ios16/gpu-plugin.ref; PLUGIN_BASE=build builds it in the arm's directory) or, for the m2v arm, PLUGIN_M2V (default .cache/fajita-out-m2v).
# Arms (boot args on top of the Wi-Fi ones wifi-test.sh uses, and fajita-ssh=1 for the dev-mode pulls):
#   b0     fajita-frame-info=0                            (the baseline: frame info off, as ios16-port)
#   fi1    fajita-frame-info=1                            (frame info, the completed generation as the frame's id)
#   fi2    fajita-frame-info=2                            (frame info, the request's own swap id)
#   timer  fajita-frame-info=0 fajita-ca-force-timer=1    (CA_FORCE_TIMER for backboardd: CADeviceUseVBL)
#   m2v    fajita-frame-info=0, the m2v plugin           (metal2vulkan draw-cost)
# KM for gfx-suite.sh is then .cache/fajita-out-gfx-ARM/unixnu/fajita-ios16/kernel/kernel.macho. ARM_SUFFIX=S builds into fajita-out-gfx-ARMS
# (two images of one arm, e.g. m2v with two plugins; gfx-arms-run.sh takes the same ARM_SUFFIX).
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
CREDS=${WIFI_CREDS:-$HOME/.config/unixnu/wifi-kittycorner}
PHYS_RESERVE=${WIFI_PHYS_RESERVE:-0xf5b00000.0x204000,0x8df00000.0x100000,0x8e000000.0x7800000,0x96500000.0x200000,0xf6000000.0x800000}
ssid=$( [ -r "$CREDS" ] && awk -F= '/^SSID=/{print $2}' "$CREDS")
mac=$(python3 "$BOARD/tools/wifi/persist-wlan-mac.py" 2>/dev/null)
wifi=(fajita-wifi=7 fajita-wifi-dump=1 native-ufs-partread=1 "phys-reserve=$PHYS_RESERVE" fajita-net=wifi fajita-wifi-iokit=1 fajita-wifid=1 fajita-ssh=1)
[ -n "$ssid" ] && wifi+=("fajita-wifi-ssid=$ssid")
[ -n "$mac" ] && wifi+=("fajita-wifi-mac=$mac")
[ $# -gt 0 ] || { sed -n 2,13p "$0"; exit 2; }
rc=0
for arm in "$@"; do
  plugin=${PLUGIN_BASE:-$BOARD/.cache/fajita-out}/metal.ngpu
  case $arm in
    b0) extra=(fajita-frame-info=0) ;;
    fi1) extra=(fajita-frame-info=1) ;;
    fi2) extra=(fajita-frame-info=2) ;;
    timer) extra=(fajita-frame-info=0 fajita-ca-force-timer=1) ;;
    m2v) extra=(fajita-frame-info=0); plugin=${PLUGIN_M2V:-$BOARD/.cache/fajita-out-m2v}/metal.ngpu ;;
    *) echo "gfx-arms-build: unknown arm $arm"; exit 2 ;;
  esac
  out=$BOARD/.cache/fajita-out-gfx-$arm${ARM_SUFFIX:-}; mkdir -p "$out"
  if [ "${PLUGIN_BASE:-}" = build ] && [ $arm != m2v ]; then   # build this tree's plugin in the arm's own directory (a clean build)
    rm -f "$out/metal.ngpu"; echo "== $arm: ${extra[*]} (plugin built here)"
  else
    [ -f "$plugin" ] || { echo "gfx-arms-build: no plugin $plugin"; exit 1; }
    cp "$plugin" "$out/metal.ngpu"
    echo "== $arm: ${extra[*]} (plugin $(sha1sum "$plugin" | cut -c1-8) from $plugin)"
  fi
  FAJITA_OUT=$out FAJITA_AUTORESET=${WIFI_AUTORESET:-900} "$TD/../build.sh" --test --skip-plugin "${wifi[@]}" "${extra[@]}" > "$out/arm-build.txt" 2>&1 \
    || { echo "$arm: BUILD FAILED ($out/arm-build.txt)"; tail -4 "$out/arm-build.txt"; rc=1; continue; }
  grep -a '^\[' "$out/arm-build.txt" | tail -1
done
exit $rc
