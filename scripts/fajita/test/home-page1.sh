#!/bin/bash
# home-page1.sh: bring the phone to the first home page (from an app, the App Library or another page) and check it against
# $FAJITA_WORK/home-base.png (the icon rows above the dock: the widgets, Photos, App Store, Settings). Exit 0 on page 1, 1 if it never got there.
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; S=$W; H=$TD/hc
onpage1() { T=30 $H screenshot $S/.page1-check.png >/dev/null 2>&1 || return 1
  n=$(python3 -I $TD/scrdiff.py $S/home-base.png $S/.page1-check.png 0 90 540 260 | awk '{print $4}'); [ "${n:-99999}" -lt 2000 ]; }
for step in up up right right right; do
  onpage1 && exit 0
  $H kcmd touch swipe $step >/dev/null; sleep 2
done
onpage1 && exit 0
echo "!! home-page1: not on the first home page after home gestures and swipes (see $S/.page1-check.png)" >&2; exit 1
