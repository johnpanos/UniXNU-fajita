#!/bin/bash
# home-page1.sh [--rebase]: bring the phone to the first home page (from an app, the App Library or another page) and check it against
# $FAJITA_WORK/home-base.png. Exit 0 on page 1, 1 if it never got there.
#   The way to page 1 does not depend on the icon layout (the Data partition persists and its pages reflow when apps are installed or
#   moved): home gesture, then swipe right until the screen stops changing (the Today view, left of page 1), then one swipe left.
#   --rebase: go there that way and save the references for the current Data: home-base.png (page 1) and applib-base.png (the App
#   Library: swipe left from page 1 until the screen stops changing). Run it once per boot of new Data, before fps.sh.
#   --applib: go on to the App Library (fps.sh, gfx-suite.sh).
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; S=$W; H=$TD/hc
shot() { T=30 $H screenshot "$1" >/dev/null 2>&1; }
# pixels over 8 that differ below the status bar and above the dock (the dock is on every page)
differ() { python3 -I $TD/scrdiff.py "$1" "$2" 0 60 540 1000 | awk '{print $4}'; }
# page 1 = the reference by tiles (pagematch.py): widgets, the Clock icon and the status bar redraw on their own, so a pixel count
# called page 1 "different" whenever a widget changed, and could not tell page 2 (same wallpaper) from page 1 with a redrawn widget
ispage1() { python3 -I $TD/pagematch.py $S/home-base.png "$1" >/dev/null; }
onpage1() { shot $S/.page1-check.png || return 1; ispage1 $S/.page1-check.png; }
# swipe DIR until two shots in a row match (at most 8 swipes); the last shot is in $S/.edge.png
toEdge() {
  shot $S/.edge.png
  for i in 1 2 3 4 5 6 7 8; do
    $H kcmd touch swipe $1 >/dev/null; sleep 1.5
    cp $S/.edge.png $S/.edge-prev.png; shot $S/.edge.png
    n=$(differ $S/.edge-prev.png $S/.edge.png); [ "${n:-999999}" -lt 15000 ] && return 0
  done
  return 1
}
toPage1() {
  $H kcmd touch swipe up >/dev/null; sleep 1.5; $H kcmd touch swipe up >/dev/null; sleep 1.5
  toEdge right || return 1
  # the right edge is the Today view on most Data, but page 1 itself when there is no Today view: then stay
  [ -f $S/home-base.png ] && ispage1 $S/.edge.png && return 0
  $H kcmd touch swipe left >/dev/null; sleep 2
}
# --applib: to page 1, then left until the App Library's search field ("App Library" over the blurred wallpaper) matches applib-base.png's
# (only the field: the folders below it reorder as the App Library's categories change with use). Exit 1 if it is not reached.
if [ "$1" = --applib ]; then
  onpage1 || toPage1
  for i in 1 2 3 4 5; do
    $H kcmd touch swipe left >/dev/null; sleep 2; shot $S/.applib-check.png
    n=$(python3 -I $TD/scrdiff.py $S/applib-base.png $S/.applib-check.png 30 88 510 155 | awk '{print $4}'); [ "${n:-99999}" -lt 3000 ] && exit 0
  done
  echo "!! home-page1: the App Library was not reached (see $S/.applib-check.png against $S/applib-base.png)" >&2; exit 1
fi
if [ "$1" = --rebase ]; then
  toPage1 || { echo "!! home-page1: the Today view edge was not found" >&2; exit 1; }
  shot $S/home-base.png && echo "saved $S/home-base.png"
  toEdge left || { echo "!! home-page1: the App Library edge was not found" >&2; exit 1; }
  cp $S/.edge.png $S/applib-base.png && echo "saved $S/applib-base.png"
  toPage1; exit 0
fi
onpage1 && exit 0
toPage1; onpage1 && exit 0
echo "!! home-page1: not on the first home page (see $S/.page1-check.png against $S/home-base.png; --rebase saves new references)" >&2; exit 1
