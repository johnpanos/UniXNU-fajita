#!/bin/bash
# settings-scroll.sh NAME [N]: go to the first home page (home-page1.sh), open Settings (its icon found on screen by find-icon.py), check that Settings is on
# screen, scroll its list up N times and back down (slow drags, no fling), screenshot after each step into corr/NAME-*.png, then go home.
# Exit 2 (and no scroll screenshots) when it cannot reach page 1 or Settings does not open, so a run never compares the wrong screen.
# STAY=1: stay in Settings at the end (N=0 with STAY=1 only opens it: gfx-suite.sh).
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; S=$W; H=$TD/hc; N=${2:-4}; mkdir -p $S/corr
rm -f $S/corr/$1-*.png
$TD/home-page1.sh || { echo "$1: !! not on home page 1; settings pass skipped"; exit 2; }
# Settings is open when the icon rows above the dock are neither page 1's nor the App Library's, and the dock is gone.
insettings() { T=30 $H screenshot $S/corr/$1-0.png >/dev/null 2>&1 || return 1
  a=$(python3 -I $TD/scrdiff.py $S/home-base.png $S/corr/$1-0.png 0 90 540 260 | awk '{print $4}')
  b=$(python3 -I $TD/scrdiff.py $S/applib-base.png $S/corr/$1-0.png 0 90 540 260 | awk '{print $4}')
  d=$(python3 -I $TD/scrdiff.py $S/home-base.png $S/corr/$1-0.png 0 1020 540 1150 | awk '{print $4}')
  [ "${a:-0}" -gt 30000 ] && [ "${b:-0}" -gt 30000 ] && [ "${d:-0}" -gt 20000 ]; }
ok=0
# The icon is found on the screen (find-icon.py, the layout changes as apps are installed); 664,840 when it is not found.
for try in 1 2; do
  T=30 $H screenshot $S/.settings-find.png >/dev/null 2>&1; xy=$(python3 -I $TD/find-icon.py $S/.settings-find.png | awk '{print $1, $2}'); [ -n "$xy" ] || xy="664 840"
  $H kcmd touch tap $xy >/dev/null; sleep 3; insettings $1 && { ok=1; break; }; $TD/home-page1.sh >/dev/null 2>&1; done
[ $ok = 1 ] || { echo "$1: !! Settings did not open (last screen $S/corr/$1-0.png); settings pass skipped"; exit 2; }
for i in $(seq 1 $N); do $H kcmd touch swipe xy 540 1900 540 900 60 >/dev/null; sleep 2; T=30 $H screenshot $S/corr/$1-u$i.png >/dev/null; done
for i in $(seq 1 $N); do $H kcmd touch swipe xy 540 900 540 1900 60 >/dev/null; sleep 2; T=30 $H screenshot $S/corr/$1-d$i.png >/dev/null; done
[ "${STAY:-0}" = 1 ] || { $H kcmd touch swipe up >/dev/null; sleep 2; }
echo "$1: Settings opened, $(ls $S/corr/$1-*.png | wc -l) screenshots"
