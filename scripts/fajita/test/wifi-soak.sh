#!/bin/bash
# wifi-soak.sh: N consecutive wifi-test.sh boots of one image, one table row per boot (result, every connect try with
# its BSS/band, the BSS joined). The first boot builds (unless WIFI_NO_BUILD=1 is already set), the rest reuse the image.
# Every boot is a separate held session that ends by itself (no WIFI_HOLD). Run it through bg.sh:
#
#   scripts/fajita/test/bg.sh start wifi-soak --max 5400 scripts/fajita/test/wifi-soak.sh 8
#   WIFI_EXTRA_ARGS="fajita-wifi-band=2g" scripts/fajita/test/wifi-soak.sh 4 modem link servers wlfw scan assoc
#
# Default stages: modem link servers wlfw scan assoc dhcp ping. Table: .cache/fajita-work/logs/wifi-soak/<time>.txt
# (FAJITA_OUT, WIFI_EXTRA_ARGS and the other wifi-test.sh variables pass through).
set -u
[ $# -ge 1 ] && [[ $1 =~ ^[0-9]+$ ]] || { sed -n '2,10p' "$0"; exit 2; }
N=$1; shift
STAGES=("$@"); [ ${#STAGES[@]} -eq 0 ] && STAGES=(modem link servers wlfw scan assoc dhcp ping)
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}
OUTD=$W/logs/wifi-soak; mkdir -p "$OUTD"; T=$OUTD/$(date +%Y%m%d-%H%M%S).txt
say() { echo "$*" | tee -a "$T"; }
say "wifi-soak: $N boots, stages ${STAGES[*]}, extra args '${WIFI_EXTRA_ARGS:-}' ($(git -C "$ROOT" rev-parse --short HEAD))"
pass=0
for i in $(seq 1 "$N"); do
  if [ "$i" -gt 1 ]; then export WIFI_NO_BUILD=1; fi
  "$TD/wifi-test.sh" "${STAGES[@]}" > "${T%.txt}-run$i.log" 2>&1; rc=$?
  con=$(grep -o 'console: [^ ]*' "${T%.txt}-run$i.log" | tail -1 | cut -d' ' -f2)
  tries=""; joined="none"
  if [ -f "$con" ]; then
    tries=$(grep -a -o -E 'FW7 connect #[0-9]+ .* rc -?[0-9]+: [0-9a-f:]+ [0-9]+ MHz -?[0-9]+ dBm' "$con" |
            sed -E 's/FW7 connect (#[0-9]+) .*: ([0-9a-f:]+) ([0-9]+) MHz (-?[0-9]+) dBm/\1 \2 \3MHz \4dBm/' | paste -sd ';' -)
    joined=$(grep -a -o -E 'FW7 scan #[0-9]+ connected bss [0-9]+ frames [0-9]+ full [0-9]+: [^ ]+ [0-9a-f:]+ [0-9]+ MHz -?[0-9]+ dBm' "$con" |
             head -1 | awk '{print $(NF-4), $(NF-3)"MHz", $(NF-1)"dBm"}')
    [ -z "$joined" ] && joined="none"
  fi
  fails=$(grep -E '^ *[a-z]+ +FAIL' "${T%.txt}-run$i.log" | awk '{print $1}' | paste -sd, -)
  [ "$rc" -eq 0 ] && { r=PASS; pass=$((pass + 1)); } || r="FAIL(${fails:-rc $rc})"
  say "boot $i: $r | joined $joined | tries $tries | $(basename "$(dirname "${con:-x/x}")")"
done
say "wifi-soak: $pass/$N passed"
[ "$pass" -eq "$N" ]
