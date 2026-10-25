#!/bin/bash
# corr-tour.sh NAME: the correctness tour for a GPU change, screenshots into $FAJITA_WORK/corr/NAME-*.png, in a running session:
#   settings-scroll.sh NAME (Settings opened, scrolled up and down), the App Library (opened from page 1, then two drags), a lock and
#   unlock (kcmd key power, then a swipe up), and Notes and Calendar opened from page 1 (icons found by find-icon.py against templates
#   cut from the first home page; a template is cut once into $FAJITA_WORK/icon-APP.png from page 1's known slots).
# Compare two runs with corr-tour.sh --diff A B: per screenshot, max channel difference and pixels over 8 (below the status bar).
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W/corr"; H=$TD/hc
if [ "$1" = --diff ]; then
  for a in $W/corr/$2-*.png; do b=$W/corr/$3-${a#$W/corr/$2-}; [ -f "$b" ] || continue
    python3 -I -c "
from PIL import Image,ImageChops
a=Image.open('$a').convert('RGB').crop((0,60,540,1170)); b=Image.open('$b').convert('RGB').crop((0,60,540,1170))
px=list(ImageChops.difference(a,b).get_flattened_data()); print('%-28s maxdiff %3d px>8 %d' % ('${a#$W/corr/$2-}', max(max(p) for p in px), sum(1 for p in px if max(p)>8)))"
  done; exit 0; fi
[ -n "$1" ] || { sed -n 2,6p "$0"; exit 2; }
N=$1; shot() { T=30 $H screenshot $W/corr/$N-$1.png >/dev/null 2>&1; }
$TD/settings-scroll.sh $N 3
# App Library: page 1, swipe left (the stock layout has one home page), then drags.
$TD/home-page1.sh >/dev/null 2>&1; shot home
$H kcmd touch swipe left >/dev/null; sleep 2; shot applib
$H kcmd touch swipe xy 540 1900 540 900 60 >/dev/null; sleep 2; shot applib-u1
$H kcmd touch swipe xy 540 900 540 1900 60 >/dev/null; sleep 2; shot applib-d1
# Lock and unlock.
$H kcmd key power >/dev/null; sleep 2; $H kcmd key power >/dev/null; sleep 2; shot lock
$H kcmd touch swipe up >/dev/null; sleep 3; shot unlocked
# Apps from page 1: Notes (row 2, column 1) and Calendar (row 1, column 2) on the stock layout, as half-resolution 80x80 templates.
for app in notes:41,515 calendar:166,378; do
  name=${app%%:*}; at=${app#*:}; $TD/home-page1.sh >/dev/null 2>&1; T=30 $H screenshot $W/.tour.png >/dev/null 2>&1
  [ -f $W/icon-$name.png ] || python3 -I -c "
from PIL import Image; x,y=$at; Image.open('$W/.tour.png').convert('RGB').crop((x,y,x+80,y+80)).save('$W/icon-$name.png')"
  xy=$(python3 -I $TD/find-icon.py $W/.tour.png $W/icon-$name.png | awk '{print $1, $2}')
  [ -n "$xy" ] || { echo "$N: !! $name icon not found"; continue; }
  $H kcmd touch tap $xy >/dev/null; sleep 4; shot $name
  $H kcmd touch swipe up >/dev/null; sleep 2
done
$TD/home-page1.sh >/dev/null 2>&1
echo "$N: $(ls $W/corr/$N-*.png | wc -l) screenshots in $W/corr"
