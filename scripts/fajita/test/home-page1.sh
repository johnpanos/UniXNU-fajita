#!/bin/bash
# home-page1.sh [--rebase]: bring the phone to the first home page (from an app, the App Library or another page) and check it against
# $FAJITA_WORK/home-base.png. Exit 0 on page 1, 1 if it never got there.
#   The way to page 1 does not depend on the icon layout (the Data partition persists and its pages reflow when apps are installed or
#   moved): home gesture, then swipe right until the screen stops changing (the Today view, left of page 1), then one swipe left.
#   --rebase: go there that way and save the references for the current Data: home-base.png (page 1) and applib-base.png (the App
#   Library: swipe left from page 1 until the screen stops changing). Run it once per boot of new Data, before fps.sh.
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; S=$W; H=$TD/hc
shot() { T=30 $H screenshot "$1" >/dev/null 2>&1; }
# pixels over 8 that differ below the status bar and above the dock (the dock is on every page)
differ() { python3 -I $TD/scrdiff.py "$1" "$2" 0 60 540 1000 | awk '{print $4}'; }
onpage1() { shot $S/.page1-check.png || return 1; n=$(differ $S/home-base.png $S/.page1-check.png); [ "${n:-999999}" -lt 15000 ]; }
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
  $H kcmd touch swipe left >/dev/null; sleep 2
}
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
