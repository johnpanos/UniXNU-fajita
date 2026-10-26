#!/bin/bash
# fps.sh: the two standard rates, reported side by side:
#   home   4 plain swipes and 4 60-frame swipes, page 1 <-> page 2 (starts from home page 1, home-page1.sh)
#   applib APPLIB_N (default 8) 60-frame scrolls of the App Library's blurred list (up/down drags; reached by swiping left from page 1)
# Each rate is frames over swipe-rate.py's window (the swipe and SETTLE s after its lift, idle time included: the render server presents
# only what changes, so a perfect 60 Hz animation reads well under 60 there); "motion" is the rate over the stretch where frames come
# under 50 ms apart (swipe-rate.py), the frame rate while something moves: the median over the phase's swipes, [min-max].
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; G=$W; H=$TD/hc
mean() { python3 -I -c "
import sys,re
p=[];s=[];mo=[]
for l in open('$1'):
    m=re.match(r'\s*(\S+): \d+ frames in [\d.]+ s = ([\d.]+) fps',l)
    if m: (s if ':' in m.group(1) else p).append(float(m.group(2)))
    m=re.search(r'\| motion ([\d.]+) fps',l)
    if m: mo.append(float(m.group(1)))
f=lambda v: '%.1f (n=%d)'%(sum(v)/len(v),len(v)) if v else 'n/a'
mo.sort()
print('$2', f(p), '| 60-frame', f(s), '| motion %.1f [%.1f-%.1f]'%(mo[len(mo)//2] if len(mo)%2 else (mo[len(mo)//2-1]+mo[len(mo)//2])/2,mo[0],mo[-1]) if mo else '')"; }
# A phase whose swipes lost the screen (a swipe with under 5 frames: a locked screen, the wrong page, a lost touch) is run again once,
# and reported INVALID if it fails again, so a navigation slip never reads as a frame rate.
valid() { [ -s "$1" ] && ! grep -q 'not queued\|App Library not reached' "$1" && python3 -I -c "
import re,sys
n=[int(m.group(1)) for l in open('$1') for m in [re.match(r'\s*\S+: (\d+) frames in',l)] if m]
sys.exit(0 if n and min(n)>=5 else 1)"; }
home() { $TD/home-page1.sh >/dev/null 2>&1 || echo 'fps.sh: !! not on home page 1'
  (cd /tmp && SETTLE=1.0 timeout 200 python3 -I $TD/swipe-rate.py left right left right left:60 right:60 left:60 right:60 2>&1 | grep -v Deprec) > $G/fps-last.txt; }
home; valid $G/fps-last.txt || { echo 'fps.sh: home phase invalid, running it again'; home; }
valid $G/fps-last.txt || HOMEBAD=1
# The App Library: swipe left from page 1 until the screen's search field ("App Library" over the blurred wallpaper) matches
# applib-base.png's. Only the field is compared: the folders below it reorder as the App Library's categories change with use.
applib() { inlib=0; $TD/home-page1.sh --applib >/dev/null 2>&1 && inlib=1
if [ $inlib = 1 ]; then
  lib=""; for i in $(seq 1 $(( ${APPLIB_N:-8} / 2 ))); do lib="$lib xy,540,1900,540,900:60 xy,540,900,540,1900:60"; done
  (cd /tmp && SETTLE=1.0 timeout 400 python3 -I $TD/swipe-rate.py $lib 2>&1 | grep -v Deprec) > $G/fps-applib.txt
else echo 'fps.sh: !! App Library not reached' > $G/fps-applib.txt; fi; }
applib; valid $G/fps-applib.txt || { echo 'fps.sh: App Library phase invalid, running it again'; applib; }
valid $G/fps-applib.txt || LIBBAD=1
$TD/home-page1.sh >/dev/null 2>&1
echo "FPS home: $([ -n "$HOMEBAD" ] && echo INVALID || mean $G/fps-last.txt plain) || App Library: $([ -n "$LIBBAD" ] && echo INVALID || mean $G/fps-applib.txt scroll | sed 's/scroll n\/a | 60-frame/scroll/')"
