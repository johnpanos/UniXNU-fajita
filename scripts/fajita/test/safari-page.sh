#!/bin/bash
# safari-page.sh OUT.png [open|reload|apple|wake]: on a held session (session.sh start / wifi-test.sh with WIFI_HOLD=1), show Safari's
# page and screenshot it to OUT.png, then print the plugin's surface probe rows (SI: surfaces backboardd imported, HC: host copies).
#   open    (default) wake, unlock, tap Safari in the dock (its last page loads)
#   apple   as open, then tap the Apple favorite (a new tab shows the favorites)
#   reload  tap Safari's reload button (Safari already in front)
#   wake    only wake and unlock
# The screen may be off (a screenshot then shows an old frame): the wake presses power and swipes up until the swipe changes the screen.
set -uo pipefail
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd)
OUT=${1:?usage: safari-page.sh OUT.png [open|reload|apple|wake]}; MODE=${2:-open}
S="$TD/session.sh"
KM=${KM:-$(ls -t "$ROOT"/.cache/fajita-out*/unixnu/fajita-ios16/kernel/kernel.macho 2>/dev/null | head -1)}
shot() { "$S" call screenshot "$1" >/dev/null 2>&1; }
wake() {   # power, then a swipe up; a swipe that changes nothing means the screen was off (or locked again): once more, at most 3 times
  local a=$OUT.wake-a.png b=$OUT.wake-b.png i
  for i in 1 2 3; do
    "$S" call kcmd key power >/dev/null 2>&1; sleep 2; shot "$a"
    "$S" call kcmd touch swipe up >/dev/null 2>&1; sleep 3; shot "$b"
    cmp -s "$a" "$b" || break
  done
  rm -f "$a" "$b"
}
case $MODE in
  open|apple) wake; "$S" call kcmd touch tap 414 2176 >/dev/null 2>&1; sleep 7
              [ "$MODE" = apple ] && { "$S" call kcmd touch tap 132 456 >/dev/null 2>&1; sleep 8; } ;;
  reload) "$S" call kcmd touch tap 952 2044 >/dev/null 2>&1; sleep 8 ;;
  wake) wake ;;
  *) echo "unknown mode $MODE"; exit 2 ;;
esac
shot "$OUT" && echo "screenshot $OUT"
[ -n "$KM" ] && KM=$KM timeout 30 python3 "$TD/rows.py" 2>/dev/null | grep -E '^(SI|HC|F |N surface|N no-copy)' || true
