#!/bin/bash
# brightness-check.sh: on a held session, prove that iOS drives the panel brightness: open Control Center, drag its brightness slider
# up, then down, and read the panel's DBV back after each (kcmd brightness read, DCS 0x52). PASS when the panel follows both ways
# (up > down + 100). Screenshots in $FAJITA_WORK/brightness/{cc,up,down}.png. Ends with Control Center closed.
# Slider geometry is the iPhone X Control Center on the 1080x2340 panel (brightness column x 660, y 870..1280).
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; H=$TD/hc; S=$W/brightness; mkdir -p "$S"
dbv() { $H kcmd brightness read | sed -n 's/.*dbv \([0-9]*\).*/\1/p'; }
$H kcmd brightness | grep -q "fajita-brightness=0" && { echo "FAIL: fajita-brightness=0 in this build"; exit 1; }
b0=$(dbv)
$H kcmd touch swipe xy 1000 5 1000 1300 30 >/dev/null; sleep 2; T=30 $H screenshot "$S/cc.png" >/dev/null
$H kcmd touch swipe xy 660 1200 660 870 40 >/dev/null; sleep 1.5; up=$(dbv); T=30 $H screenshot "$S/up.png" >/dev/null
$H kcmd touch swipe xy 660 950 660 1270 40 >/dev/null; sleep 1.5; down=$(dbv); T=30 $H screenshot "$S/down.png" >/dev/null
$H kcmd touch swipe up >/dev/null
echo "panel DBV: start ${b0:-?} after slider up ${up:-?} after slider down ${down:-?}"
[ -n "$up" ] && [ -n "$down" ] && [ "$up" -gt $((down + 100)) ] && { echo PASS; exit 0; }
echo "FAIL: the panel did not follow the slider"; exit 1
