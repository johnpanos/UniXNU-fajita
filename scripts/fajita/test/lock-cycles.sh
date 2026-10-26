#!/bin/bash
# lock-cycles.sh [CYCLES] [IDLE_S]: on a held session with an app in front (e.g. safari-page.sh open), lock the phone with the power key,
# leave it IDLE_S seconds (default 20; the screen goes blank: the display's blank swaps), wake and unlock, then check the display presents
# (two screenshots around a small scroll differ) and that no "swap_wait_gated" timeout appeared. The user-hn-freeze scenario: Safari open,
# dim/lock, wake. Prints PASS/FAIL per cycle, exit 1 if any failed; screenshots in $FAJITA_WORK/lock-cycles/.
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd); S=$TD/session.sh
W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}; OUT=$W/lock-cycles; mkdir -p "$OUT"
CYCLES=${1:-4}; IDLE=${2:-20}
CON=$(ls -td "$W"/logs/session-*/ 2>/dev/null | while read -r d; do [ -f "$d/usb-console.txt" ] && { echo "$d/usb-console.txt"; break; }; done)
shot() { "$S" call screenshot "$1" >/dev/null 2>&1; }
fails=0
for c in $(seq 1 "$CYCLES"); do
  waits0=$(grep -a -c 'swap_wait_gated' "$CON" 2>/dev/null)
  "$S" call kcmd key power >/dev/null 2>&1; sleep "$IDLE"
  presented=0; t0=$(date +%s)
  while [ $(( $(date +%s) - t0 )) -lt 30 ]; do
    "$S" call kcmd key power >/dev/null 2>&1; sleep 2; "$S" call kcmd touch swipe up >/dev/null 2>&1; sleep 3
    shot "$OUT/c$c-a.png"; "$S" call kcmd touch swipe up >/dev/null 2>&1; sleep 2; shot "$OUT/c$c-b.png"
    cmp -s "$OUT/c$c-a.png" "$OUT/c$c-b.png" || { presented=1; break; }
  done
  waits=$(( $(grep -a -c 'swap_wait_gated' "$CON" 2>/dev/null) - waits0 ))
  if [ $presented = 1 ] && [ "$waits" = 0 ]; then echo "cycle $c: PASS presents after wake; swap_wait timeouts 0"
  else echo "cycle $c: FAIL presents: $presented; swap_wait timeouts $waits"; fails=$((fails+1)); fi
done
exit $((fails > 0))
