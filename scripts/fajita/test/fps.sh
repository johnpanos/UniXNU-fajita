#!/bin/bash
# fps.sh: the two standard rates, reported side by side:
#   home   4 plain swipes and 4 60-frame swipes, page 1 <-> page 2 (starts from home page 1, home-page1.sh)
#   applib 4 60-frame scrolls of the App Library's blurred list (up/down drags; reached by swiping left from page 1)
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; G=$W; H=$TD/hc
mean() { python3 -I -c "
import sys,re
p=[];s=[]
for l in open('$1'):
    m=re.match(r'\s*(\S+): \d+ frames in [\d.]+ s = ([\d.]+) fps',l)
    if m: (s if ':' in m.group(1) else p).append(float(m.group(2)))
f=lambda v: '%.1f (n=%d)'%(sum(v)/len(v),len(v)) if v else 'n/a'
print('$2', f(p), '| 60-frame', f(s))"; }
$TD/home-page1.sh >/dev/null 2>&1 || echo 'fps.sh: !! not on home page 1'
(cd /tmp && SETTLE=1.0 timeout 200 python3 -I $TD/swipe-rate.py left right left right left:60 right:60 left:60 right:60 2>&1 | grep -v Deprec) > $G/fps-last.txt
# The App Library: swipe left from page 1 until the screen matches applib-base.png above the list.
$TD/home-page1.sh >/dev/null 2>&1; inlib=0
for i in 1 2 3 4 5; do
  $H kcmd touch swipe left >/dev/null; sleep 2; T=30 $H screenshot $G/.applib-check.png >/dev/null 2>&1
  n=$(python3 -I $TD/scrdiff.py $G/applib-base.png $G/.applib-check.png 0 90 540 260 | awk '{print $4}'); [ "${n:-99999}" -lt 4000 ] && { inlib=1; break; }
done
if [ $inlib = 1 ]; then
  (cd /tmp && SETTLE=1.0 timeout 200 python3 -I $TD/swipe-rate.py xy,540,1900,540,900:60 xy,540,900,540,1900:60 xy,540,1900,540,900:60 xy,540,900,540,1900:60 2>&1 | grep -v Deprec) > $G/fps-applib.txt
else echo 'fps.sh: !! App Library not reached' > $G/fps-applib.txt; fi
$TD/home-page1.sh >/dev/null 2>&1
echo "FPS home: $(mean $G/fps-last.txt plain) || App Library: $(mean $G/fps-applib.txt scroll | sed 's/scroll n\/a | 60-frame/scroll/')"
