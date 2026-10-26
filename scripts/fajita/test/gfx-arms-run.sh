#!/bin/bash
# gfx-arms-run.sh ARM...: measure each gfx arm (built by gfx-arms-build.sh into .cache/fajita-out-gfx-ARM) on its own boot: boot it with
# Wi-Fi (wifi-test.sh, WIFI_NO_BUILD, the session held), run the full suite REPEAT times (default 2) as run ARM-RUNTAG, end the session.
# Then `gfx-report.py --compare $FAJITA_WORK/gfx/b0f $FAJITA_WORK/gfx/ARM-RUNTAG` gives the before/after rows. Each arm's image must carry
# an autoreset longer than a boot plus the suite (gfx-arms-build.sh with WIFI_AUTORESET=3000); gfx-suite.sh refuses otherwise.
#   Env: REPEAT (2), RUNTAG (default the date, MMDD), SCENES (default all), CAWATCH (passed to the suite).
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}
[ $# -gt 0 ] || { sed -n 2,6p "$0"; exit 2; }
tag=${RUNTAG:-$(date +%m%d)}; rc=0
for arm in "$@"; do
  out=$BOARD/.cache/fajita-out-gfx-$arm
  [ -f "$out/payload.img" ] || { echo "== $arm: no image in $out (gfx-arms-build.sh $arm)"; rc=1; continue; }
  echo "== $arm: boot $(date +%T)"
  log=$W/gfx/arm-$arm-$tag-boot.txt; mkdir -p $W/gfx
  FAJITA_OUT=$out WIFI_NO_BUILD=1 WIFI_HOLD=1 $TD/wifi-test.sh modem link servers wlfw scan assoc dhcp ping > $log 2>&1
  sess=$(grep -o 'session-[0-9]\{6\}' $log | tail -1)
  if ! grep -q 'PASS: all requested stages' $log; then
    echo "== $arm: boot or Wi-Fi failed ($log)"; tail -3 $log; [ -n "$sess" ] && T=60 $TD/session.sh end $sess >/dev/null 2>&1; rc=1; continue
  fi
  echo "== $arm: $sess up, suite $arm-$tag"
  KM=$out/unixnu/fajita-ios16/kernel/kernel.macho REPEAT=${REPEAT:-2} $TD/gfx-suite.sh $arm-$tag ${SCENES:-} > $W/gfx/arm-$arm-$tag-suite.txt 2>&1 || rc=1
  grep -E '^ALL|^\[.*\] !!' $W/gfx/$arm-$tag/report.txt $W/gfx/$arm-$tag/suite.log 2>/dev/null | head -5
  T=60 $TD/session.sh end $sess >/dev/null 2>&1
  echo "== $arm: done $(date +%T)"
done
exit $rc
