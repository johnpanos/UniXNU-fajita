#!/bin/bash
# gpu-ab.sh ENVFILE... : A/B several plugin configurations in ONE boot, with no CI build or reboot between them. For each gpu-env
# file: push it to /private/var/ngpu/gpu-env, restart backboardd (ngpu-metal killall; launchd relaunches it and it rediscovers
# Metal), wait until backboardd is back and settled, run the swipe sampler, screenshot the home screen and diff it against
# $FAJITA_WORK/home-base.png (the first home page; home-page1.sh goes there first). Prints one summary row per variant and appends it to $FAJITA_WORK/ab-results.txt.
# Needs a running GPU session (gpu-status.sh) whose kernel has fajita-gpu=11 and kcmd touch. Env: SPECS (swipes, default
# "left right left:60 right:60"), SETTLE_S (wait after restart, default 25), BASE (reference png),
# FPS=1 (fps.sh, home and App Library, instead of the swipe sampler).
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; S=$W; G=$W
H() { python3 -I "$(dirname "$0")/../../fajita-usbdbg-hub.py" call --timeout ${T:-20} "$@"; }
BASE=${BASE:-$G/home-base.png}; OUT=$G/ab-results.txt
$TD/gpu-status.sh >/dev/null; [ $? -eq 2 ] && { echo "no run holds the phone"; exit 1; }
# ROUNDS=N (default 1): the variants N times, interleaved (A B A B ...), so drift over the boot cancels; a summary at the end gives each
# variant's median and spread of home and App Library motion fps over its rounds.
RUN=$G/.ab-run-$$; : > $RUN
list=(); for r in $(seq 1 ${ROUNDS:-1}); do list+=("$@"); done
for env in "${list[@]}"; do
  name=$(basename $env); echo "=== $name: $(tr '\n' ' ' < $env)"
  T=60 H push $(realpath $env) /private/var/ngpu/gpu-env 644 >/dev/null 2>&1   # the agent may answer "eof"; the pull checks it
  T=60 H pull /private/var/ngpu/gpu-env $G/ab-env-check >/dev/null 2>&1; cmp -s $env $G/ab-env-check || { echo "$name: push failed"; continue; }
  T=30 H run /usr/sbin/ngpu-metal killall backboardd >/dev/null
  up=0; for i in $(seq 1 20); do sleep 3; T=15 H run /usr/sbin/ngpu-metal ps 2>/dev/null | grep -q backboardd && { up=1; break; }; done
  [ $up = 1 ] || { echo "$name: backboardd did not come back"; echo "$(date +%T) $name DEAD" >> $OUT; continue; }
  sleep ${SETTLE_S:-25}
  # correctness: the lock screen the restart brings up, below the clock, against lock-off.png (the all-skips-off baseline)
  T=30 H screenshot $G/lock-ab-$name.png >/dev/null
  lchk=$(python3 -I -c "
from PIL import Image,ImageChops
a=Image.open('$G/lock-off.png').convert('RGB').crop((0,300,540,1170)); b=Image.open('$G/lock-ab-$name.png').convert('RGB').crop((0,300,540,1170))
px=list(ImageChops.difference(a,b).get_flattened_data()); print('lock maxdiff %d px>8 %d' % (max(max(p) for p in px), sum(1 for p in px if max(p)>8)))" 2>&1)
  T=8 H kcmd touch swipe up >/dev/null; sleep 2   # wake / unlock to the home screen
  if [ "${FPS:-0}" = 1 ]; then   # FPS=1: the standard benchmark (fps.sh: home and App Library) instead of the swipe sampler
    rate=$(timeout 300 $TD/fps.sh 2>&1); echo "$rate" | grep "FPS "
    fps=$(echo "$rate" | grep "^FPS " | sed 's/^FPS //; s/ (n=[0-9]*)//g')
  else
  rate=$(cd /tmp && SETTLE=1.0 timeout 150 python3 -I $TD/swipe-rate.py ${SPECS:-left right left:60 right:60} 2>&1 | grep -v Deprec)
  echo "$rate" | tail -6
  fps=$(echo "$rate" | grep -o "MEAN: [0-9.]*" | grep -o "[0-9.]*$")   # swipe-rate.py's mean over the swipes
  fi
  perf=$(python3 -I $TD/perf.py left:100 900 2>/dev/null | sed -n 1,2p | tr '\n' ' ' | grep -o "gpu busy [0-9]*%\|alu [0-9]*% of busy" | tr '\n' ' ')
  # The home check compares the FIRST home page: the swipes above end on whichever page they reached (often the App Library).
  sleep 1.5; pg=""; $TD/home-page1.sh 2>/dev/null || pg=" !! NOT ON HOME PAGE 1"
  T=30 H screenshot $G/home-ab-$name.png >/dev/null
  chk=$(python3 -I -c "
from PIL import Image,ImageChops
a=Image.open('$BASE').convert('RGB').crop((0,60,540,1170)); b=Image.open('$G/home-ab-$name.png').convert('RGB').crop((0,60,540,1170))
px=list(ImageChops.difference(a,b).get_flattened_data()); print('maxdiff %d px>8 %d' % (max(max(p) for p in px), sum(1 for p in px if max(p)>8)))" 2>&1)
  # Loud when most of the screen differs: a stale BASE or the wrong screen, never a rendering verdict.
  for c in "$lchk" "$chk"; do n=$(echo "$c" | grep -o "px>8 [0-9]*" | grep -o "[0-9]*$"); [ "${n:-0}" -gt 180000 ] && pg="$pg !! >30% OF THE SCREEN DIFFERS: stale baseline or wrong screen ($c)"; done
  row="$(date +%T) $name fps ${fps:-?} | $lchk | home $chk | $perf$pg"; echo "$row"; echo "$row" >> $OUT; echo "$row" >> $RUN
  [ -n "$pg" ] && echo "$pg" >&2
done
python3 -I -c "
import re,statistics as st,collections
v=collections.defaultdict(lambda:([],[]))
for l in open('$RUN'):
    p=l.split()
    m=re.findall(r'motion ([\d.]+)',l)
    if len(p)>1 and len(m)>=2: v[p[1]][0].append(float(m[0])); v[p[1]][1].append(float(m[1]))
f=lambda x:'%.1f [%.1f-%.1f] n=%d'%(st.median(x),min(x),max(x),len(x)) if x else 'n/a'
for k,(h,a) in v.items(): print('SUMMARY %-24s home motion %s | App Library motion %s'%(k,f(h),f(a)))
" | tee -a $OUT; rm -f $RUN
