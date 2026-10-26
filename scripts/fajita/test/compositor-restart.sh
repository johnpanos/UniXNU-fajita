#!/bin/bash
# compositor-restart.sh [ROUNDS]: on a held session, kill the render server (backboardd) while frames are in flight and check that the
# display still presents afterwards. The user-hn-freeze (2026-10-26): backboardd crashed during a GPU copy of its frame, the restarted one's
# swaps were validated but never copied, and the screen stayed frozen (NativeLegacyDisplay finishTransfer kept its latch for a request the
# family had discarded).
# Per round: swipe (Safari or any page scrolls, its frames in flight), kill -9 backboardd at once, wait for launchd's new backboardd,
# then wake, unlock and swipe until two screenshots around a swipe differ (the display presents; up to 60 s), and the console must show
# no new "swap_wait_gated" timeout. The kill lands on a frame out for its GPU copy only some of the time: run several rounds. Prints PASS/FAIL per round; exit 1 if any failed. Screenshots in $FAJITA_WORK/compositor-restart/.
# Needs fjbundle (scripts/fajita/build-usbdbg-agent.sh; $FJBUNDLE, default .cache/fajita-inputs/fjbundle) for --kill.
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd); S=$TD/session.sh
W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}; OUT=$W/compositor-restart; mkdir -p "$OUT"
ROUNDS=${1:-3}
FB=${FJBUNDLE:-$ROOT/.cache/fajita-inputs/fjbundle}; [ -f "$FB" ] || { echo "no fjbundle at $FB"; exit 1; }
NAME=fjbundle-$(sha256sum "$FB" | cut -c1-8); "$S" push "$FB" /private/var/ngpu/sbin/$NAME >/dev/null 2>&1
CON=$(ls -td "$W"/logs/session-*/ 2>/dev/null | while read -r d; do [ -f "$d/usb-console.txt" ] && { echo "$d/usb-console.txt"; break; }; done)
bbpid() { "$S" run /bin/ps -axo pid,comm 2>/dev/null | awk '$2 ~ /\/backboardd$/ {print $1}' | head -1; }
shot() { "$S" call screenshot "$1" >/dev/null 2>&1; }
fails=0
for r in $(seq 1 "$ROUNDS"); do
  old=$(bbpid); waits0=$(grep -a -c 'swap_wait_gated' "$CON" 2>/dev/null)
  "$S" call kcmd touch swipe up >/dev/null 2>&1
  "$S" run /usr/sbin/$NAME --kill 9 backboardd >/dev/null 2>&1
  new=; for i in $(seq 1 20); do sleep 2; new=$(bbpid); [ -n "$new" ] && [ "$new" != "$old" ] && break; done
  if [ -z "$new" ] || [ "$new" = "$old" ]; then echo "round $r: FAIL no new backboardd (was $old)"; fails=$((fails+1)); continue; fi
  # Wake, unlock and swipe until two screenshots around a swipe differ (the display presents), for up to 60 s (the restart of
  # backboardd and SpringBoard takes 15-30 s).
  presented=0; t0=$(date +%s)
  while [ $(( $(date +%s) - t0 )) -lt 60 ]; do
    "$S" call kcmd key power >/dev/null 2>&1; sleep 2; "$S" call kcmd touch swipe up >/dev/null 2>&1; sleep 3
    shot "$OUT/r$r-a.png"; "$S" call kcmd touch swipe left >/dev/null 2>&1; sleep 3; shot "$OUT/r$r-b.png"
    cmp -s "$OUT/r$r-a.png" "$OUT/r$r-b.png" || { presented=1; break; }
  done
  waits=$(( $(grep -a -c 'swap_wait_gated' "$CON" 2>/dev/null) - waits0 ))
  lost=$(grep -a 'no longer pending' "$CON" 2>/dev/null | tail -1 | sed -n 's/.*(why \([0-9]*\), \([0-9]*\) so far).*/why \1, \2 so far/p')
  if [ $presented = 0 ] || [ "$waits" -gt 0 ]; then
    echo "round $r: FAIL backboardd $old -> $new; presents: $([ $presented = 1 ] && echo yes || echo 'no (60 s)'); swap_wait timeouts $waits; lost frames: ${lost:-none}"
    fails=$((fails+1))
  else
    echo "round $r: PASS backboardd $old -> $new; presents after $(( $(date +%s) - t0 )) s; swap_wait timeouts 0; lost frames: ${lost:-none}"
  fi
done
exit $((fails > 0))
