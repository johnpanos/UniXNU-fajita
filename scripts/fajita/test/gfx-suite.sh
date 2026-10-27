#!/bin/bash
# gfx-suite.sh RUN [SCENE...]: the graphics suite on the held session (session.sh start, or wifi-test.sh with WIFI_HOLD=1 for the web
# scenes): each scene's motion measured frame by frame on the panel's timeline (gfx-scene.py), its GPU, CPU and clocks, a screenshot
# at its rest points, and the stability counters; one report: $FAJITA_WORK/gfx/RUN/report.txt (+ report.tsv, metrics.json).
#   gfx-suite.sh ab RUN ENV_A ENV_B [ROUNDS [SCENE...]]   A/B two gpu-env files in this boot (backboardd restarted per variant, as gpu-ab.sh),
#                                              ROUNDS times interleaved; RUN-a1, RUN-b1, ... and the comparison in RUN-ab.txt
# Scenes (default all): home applib settings calendar maps web-hn web-wiki web-apple web-heavy (web-*: Wi-Fi up). Apps with real
# content on this Data: Settings, Calendar (day view), Maps; Photos and Notes are empty and the App Store cannot connect (no Apple ID).
# Env: ENV=FILE (gpu-env lines for the run; the suite adds NGPU_FRAME_TRACE=1 unless TRACE=0 and restarts backboardd once),
# DIAG=1 (measure with a diagnostic in gpu-env anyway, the report marked DIAG), WARMUP=0 (no warm-up pass), SETTLE_MAX/SETTLE_CPU/SETTLE_TEMP (the settle check before the scenes, see 2b),
# PLAN_S/FORCE=1 (the run's planned length against the session's remaining autoreset time, see below),
# CATCH=0 (no occurrence catcher: see gfx-scene.py --catch), CAWATCH=1 (the render server's display timer per scene, fjsurface --watch-ca, instead of the CPU windows: tools/gpu/ca-timer.py),
# REPEAT=N (every scene N times: SCENE, SCENE-r2, ... for the run-to-run noise, gfx-report.py --noise), REBASE=1 (save this run's
# rest screenshots as the references, $FAJITA_WORK/gfx/refs: iOS UI, never in git), NORESTART=1 (keep backboardd: no env change).
# Tools pushed to new names each run (a changed binary pushed over the same path ran the old one): ngpu-metal (cpu sampler) and
# fjbundle (--open) from $FAJITA_OUT/tools (scripts/fajita/build-usbdbg-agent.sh $FAJITA_OUT/tools/fjdbg-agent builds them).
set -uo pipefail
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}; OUTD=${FAJITA_OUT:-$BOARD/.cache/fajita-out}
SS=$TD/session.sh; H=$TD/hc
export KM=${KM:-$OUTD/unixnu/fajita-ios16/kernel/kernel.macho}
usage() { sed -n 2,15p "$0"; exit 2; }
[ $# -ge 1 ] || usage

if [ "$1" = ab ]; then
  [ $# -ge 4 ] || usage
  R=$2 A=$3 B=$4 N=${5:-1}; shift 5 2>/dev/null || shift $#
  for i in $(seq 1 $N); do
    ENV=$A $0 $R-a$i "$@" || echo "gfx-suite: !! $R-a$i failed"
    ENV=$B $0 $R-b$i "$@" || echo "gfx-suite: !! $R-b$i failed"
  done
  for i in $(seq 1 $N); do python3 -I $BOARD/tools/gpu/gfx-report.py --compare $W/gfx/$R-a$i $W/gfx/$R-b$i; done | tee $W/gfx/$R-ab.txt
  exit 0
fi

RUN=$1; shift
O=$W/gfx/$RUN; mkdir -p $O; rm -f $O/*.json $O/*.png
SCENES=${*:-home applib settings calendar maps web-hn web-wiki web-apple web-heavy}
LOGF=$W/logs/$(cat $W/session.current 2>/dev/null)/usb-console.txt
$TD/gpu-status.sh >/dev/null; [ $? -eq 2 ] && { echo "gfx-suite: no run holds the phone (session.sh start)"; exit 1; }
[ -f "$KM" ] || { echo "gfx-suite: no kernel $KM (KM=)"; exit 1; }
log() { echo "[$(date +%T)] $*" | tee -a $O/suite.log; }
: > $O/suite.log
# The session's own end: a --test image resets itself sheng-autoreset s after XNU booted (wifi-test.sh: WIFI_AUTORESET, default 900), and a
# suite that runs past it measures a dead phone (b0final, 2026-10-26: reset at 15:30, the rest of the scenes ran against fastboot). The
# remaining time is printed and a run planned longer than that is refused (PLAN_S, default 45 s a scene x REPEAT + 240 s of warm-up and
# settling; FORCE=1 runs anyway); before every scene the cycle is checked and the suite stops when it ended.
SESS=$(cat $W/session.current 2>/dev/null); CYC=$W/logs/$SESS/cycle.txt
export GFX_SESSION=$SESS GFX_BOOT="$(grep -a -m1 'XNU booting' $CYC 2>/dev/null)" GFX_PLUGIN="$(grep -a -m1 'stack\|metal.ngpu' ${KM%/unixnu/*}/build.txt 2>/dev/null | head -c 200)"
session_end() { grep -aE '^\[[0-9:]+\] RESULT:|back in fastboot' "$CYC" 2>/dev/null | head -1; }
OUTK=${KM%/unixnu/*}; AR=$(grep -o 'sheng-autoreset=[0-9]*' "$OUTK/build.txt" 2>/dev/null | tail -1 | cut -d= -f2)
BOOTT=$(grep -a -m1 'XNU booting' "$CYC" 2>/dev/null | grep -o '[0-9]\{2\}:[0-9]\{2\}:[0-9]\{2\}')
nsc=$(echo $SCENES | wc -w); PLAN=${PLAN_S:-$(( nsc * ${REPEAT:-1} * 45 + 240 ))}
if [ -n "$AR" ] && [ -n "$BOOTT" ]; then
  left=$(( $(date -d "$BOOTT" +%s) + AR - $(date +%s) ))
  log "session $SESS: booted $BOOTT, autoreset $AR s: ${left} s left, this run needs about $PLAN s"
  [ $left -lt $PLAN ] && [ "${FORCE:-0}" != 1 ] && { log "!! the session resets in $left s, before this run ($PLAN s) can finish: boot with WIFI_AUTORESET (or FAJITA_AUTORESET) larger, or FORCE=1"; exit 1; }
else
  log "session $SESS: autoreset unknown (no sheng-autoreset in $OUTK/build.txt or no boot time in $CYC)"
fi

# 1. Tools on the phone, under names that change with their contents.
push_tool() {   # push_tool NAME -> prints the phone path
  local f=$OUTD/tools/$1 h; [ -f "$f" ] || f=$BOARD/.cache/fajita-inputs/$1
  [ -f "$f" ] || { echo "gfx-suite: no $1 (scripts/fajita/build-usbdbg-agent.sh $OUTD/tools/fjdbg-agent)" >&2; return 1; }
  h=$(sha1sum "$f" | cut -c1-8); local p=/private/var/ngpu/sbin/$1-$h
  T=120 $SS push "$f" $p >/dev/null 2>&1; echo $p
}
NGPU=$(push_tool ngpu-metal) || exit 1
FJB=$(push_tool fjbundle) || exit 1
FJS=$(push_tool fjsurface) || exit 1
CAW=(); [ "${CAWATCH:-0}" = 1 ] && CAW=(--cawatch "$FJS")
# The occurrence catcher (on unless CATCH=0): a rest shot strongly different from its reference is caught while it is on screen
# (gfx-scene.py --catch: the render server's fresh render of the display, the boot age, plugin build, cache state, GPU status).
[ "${CATCH:-1}" = 1 ] && CAW+=(--catch "$FJS")
T=20 $SS run $NGPU cpu 0.2 backboardd 2>&1 | grep -q "cpu over" || { echo "gfx-suite: !! $NGPU cpu does not run"; exit 1; }
log "tools: $NGPU $FJB"

# 2. The render settings: ENV (+ the frame trace), backboardd restarted once so it reads them (gpu-ab.sh's way).
ENVF=$O/gpu-env; : > $ENVF
[ -n "${ENV:-}" ] && grep -v '^\s*#' "$ENV" | grep . >> $ENVF
[ "${TRACE:-1}" = 1 ] && echo NGPU_FRAME_TRACE=1 >> $ENVF
# The phone's gpu-env persists across backboardd restarts and boots of the same Data: a diagnostic left in it (a CPU profiler, a pass trace, a
# capture) taints every later measurement (NGPU_CPU_PROFILE=backboardd took 66-80% of a core for a day of runs, 2026-10-26). It is printed,
# and a diagnostic key there stops the run unless DIAG=1 (the run is then marked DIAG); at the end the phone gets the plain gpu-env, never a
# diagnostic one back.
DIAGKEYS='NGPU_CPU_PROFILE|NGPU_PASS_TRACE|NGPU_CAPTURE|MESA_GPU_TRACES|MESA_GPU_TRACEFILE|TU_GPU_TRACEPOINT|NGPU_SURFACE_PROBE|ngpu-trace|IR3_SHADER_DEBUG|NGPU_CPU_HELD=2'
T=30 $SS pull /private/var/ngpu/gpu-env $O/gpu-env.before >/dev/null 2>&1
log "phone gpu-env before the run: $(grep -v '^\s*$' $O/gpu-env.before 2>/dev/null | tr '\n' ' ')"
left=$(grep -v '^\s*#' $O/gpu-env.before 2>/dev/null | grep -E "$DIAGKEYS" | tr '\n' ' ')
asked=$( [ -n "${ENV:-}" ] && grep -v '^\s*#' "$ENV" | grep -E "$DIAGKEYS" | tr '\n' ' ')
diag="$asked"
if [ -n "$left" ]; then
  if [ "${NORESTART:-0}" = 1 ]; then
    [ "${DIAG:-0}" = 1 ] || { log "!! diagnostics left in the phone's gpu-env ($left) and NORESTART=1: refusing to measure (DIAG=1 measures anyway)"; exit 1; }
    diag="$left $asked"
  else log "!! diagnostics left in the phone's gpu-env ($left): replaced by this run's (backboardd restarts), the plain one restored at the end"; fi
fi
[ -n "$asked" ] && [ "${DIAG:-0}" != 1 ] && { log "!! ENV asks for diagnostics ($asked): DIAG=1 to measure with them"; exit 1; }
if [ "${NORESTART:-0}" != 1 ]; then
  T=60 $SS push $ENVF /private/var/ngpu/gpu-env 644 >/dev/null 2>&1
  T=30 $SS pull /private/var/ngpu/gpu-env $O/gpu-env.check >/dev/null 2>&1; cmp -s $ENVF $O/gpu-env.check || { echo "gfx-suite: gpu-env push failed"; exit 1; }
  T=30 $SS run /usr/sbin/ngpu-metal killall backboardd >/dev/null 2>&1
  up=0; for i in $(seq 1 20); do sleep 3; T=15 $SS run /usr/sbin/ngpu-metal ps 2>/dev/null | grep -q backboardd && { up=1; break; }; done
  [ $up = 1 ] || { echo "gfx-suite: !! backboardd did not come back"; exit 1; }
  sleep ${SETTLE_S:-25}
  $TD/safari-page.sh $O/.wake.png wake >/dev/null 2>&1; rm -f $O/.wake.png
fi
BBPID=$(T=15 $SS run /usr/sbin/ngpu-metal ps 2>/dev/null | awk '$2=="backboardd"{print $1}' | head -1)
log "env: $(tr '\n' ' ' < $ENVF) | backboardd pid ${BBPID:-?}"
python3 -I -c "
import json,sys; json.dump(dict(run='$RUN', board='$(git -C $BOARD rev-parse --short HEAD)', kernel='$(sha256sum $KM | cut -c1-12)',
  env=[l for l in open('$ENVF').read().split('\n') if l], backboardd='${BBPID:-}', scenes='$SCENES'), open('$O/meta.json','w'))"
LOG0=$(wc -l < $LOGF 2>/dev/null || echo 0)

# 2b. The same start every run (boot-to-boot noise): a warm-up (WARMUP=1, default: home swipes and the App Library once, not
# measured), then a settle check: up to SETTLE_MAX (90) s until no process but the render server uses over SETTLE_CPU (25) % of a
# core over 3 s and the hottest tsens is under SETTLE_TEMP (65) C. The start state (temperature, GPU level, the busiest processes, the
# CPU clocks) goes to start.txt and the report's header; a run that never settled says so.
if [ "${WARMUP:-1}" = 1 ]; then
  $TD/home-page1.sh >/dev/null 2>&1
  for d in left right left right; do T=10 $H kcmd touch swipe $d >/dev/null 2>&1; sleep 1.2; done
  $TD/home-page1.sh --applib >/dev/null 2>&1; $TD/home-page1.sh >/dev/null 2>&1
fi
settled=0; t0=$(date +%s)
while [ $(( $(date +%s) - t0 )) -lt ${SETTLE_MAX:-90} ]; do
  cpu=$(T=20 $SS run $NGPU cpu 3 2>/dev/null); gov=$(T=10 $H kcmd gpu gov 2>/dev/null | head -1)
  busy=$(echo "$cpu" | awk '/%  [0-9]+ /{n=$4; if(n!="backboardd" && n!~/^ngpu-metal/ && $2+0>m){m=$2+0; w=n}} END{printf "%d %s", m, w}')
  temp=$(echo "$gov" | grep -o "max [0-9.]* C" | grep -o "[0-9]*" | head -1)
  echo "[$(date +%T)] settle: busiest other process ${busy:-?}% | ${temp:-?} C | $gov" >> $O/start.txt
  [ "${busy%% *}" -lt ${SETTLE_CPU:-25} ] && [ "${temp:-99}" -lt ${SETTLE_TEMP:-65} ] && { settled=1; break; }
done
echo "$cpu" | sed -n 1,10p >> $O/start.txt
# The plugin's shader primer (NativeMetal PrimeShaders.h) compiles the OS's Metal libraries in the render server once per Data, only while
# the screen is still (and one pipeline a minute while it moves): its row says whether it is done; a run before that is flagged.
dg=$(KM=$KM timeout 30 python3 -I $TD/rows.py DIAG 2>/dev/null | tr '\n' ' ')
[ -n "$dg" ] && { log "!! the plugin reports diagnostics on: $dg"; [ "${DIAG:-0}" = 1 ] || exit 1; }
[ -n "$diag" ] && echo "DIAG $diag" >> $O/start.txt
log "start: $([ $settled = 1 ] && echo settled || echo "NOT settled after ${SETTLE_MAX:-90} s") | $(tail -1 $O/start.txt | cut -c1-60) | $(grep settle: $O/start.txt | tail -1 | cut -d'|' -f1-2 | cut -c12-)"
python3 -I -c "
import json; m=json.load(open('$O/meta.json')); m['settled']=$settled
m['start']=[l for l in open('$O/start.txt') if 'settle:' in l][-1].split('settle: ')[-1].strip()[:140]
m['diag']=' '.join(l.strip() for l in open('$O/start.txt') if l.startswith('DIAG'))[:200]; json.dump(m,open('$O/meta.json','w'))" 2>/dev/null

# 3. Scenes. Navigation happens before each capture; the capture holds only the scene's own steps.
scene() {   # scene NAME CPU-PROCS STEP...
  local name=$1 procs=$2; shift 2
  python3 -I $TD/gfx-scene.py $O/$name.json --cpu "$procs" --tool $NGPU --fjbundle $FJB "${CAW[@]}" "$@" 2>&1 | tee -a $O/suite.log
  # A background process that took over BUSY_PCT (40) % of a core in the scene's last CPU windows and still does: sample it now
  # (proc-sample.sh, 3 s) into SCENE-sample-NAME.txt, so a daemon spinning beside the scene is named with its stacks (SAMPLE=0: off).
  [ "${SAMPLE:-1}" = 1 ] || return 0
  for b in $(python3 -I -c "
import json,sys
r=json.load(open('$O/$name.json')); own=set('$procs'.split(','))|{'kernel_task','SpringBoard','launchd'}
seen={}
for c in r.get('cpu',[])[-2:]:
    for p in c['procs']:
        if p['name'] not in own and not p['name'].startswith(('ngpu-metal','fjsurface')) and p['ms']/c['ms']*100>${BUSY_PCT:-40}: seen[p['name']]=1
print(' '.join(seen))" 2>/dev/null); do
    timeout 100 $TD/proc-sample.sh $b 3 > $O/$name-sample-$b.txt 2>&1 && log "$name: $b was busy: sampled ($O/$name-sample-$b.txt)"
  done
}
home() { $TD/home-page1.sh >/dev/null 2>&1 || $TD/home-page1.sh >/dev/null 2>&1; }
open_url() { T=40 $SS run $FJB --open "$1" >/dev/null 2>&1; sleep ${2:-6}; }
TOP="tap,540,30 wait,1.5"   # a tap on the status bar scrolls a list or page to its top: a rest point that does not depend on where the drags ended
SCROLL="xy,540,1900,540,900:60 xy,540,1900,540,900:60 xy,540,900,540,1900:60 xy,540,900,540,1900:60"
WEB="xy,540,1900,540,700:40 xy,540,1900,540,700:40 xy,540,1900,540,700:40 xy,540,700,540,1900:40 xy,540,700,540,1900:40 xy,540,700,540,1900:40"
run_scene() {   # run_scene SCENE SUFFIX
  local s=$1 n=$1$2
  case $s in
    home)     home || { log "!! $n: not on home page 1"; return; }
              scene $n backboardd,SpringBoard left right left right left:60 right:60 left:60 right:60 shot,page1 ;;
    applib)   $TD/home-page1.sh --applib >/dev/null 2>&1 || { log "!! $n: App Library not reached"; return; }
              scene $n backboardd,SpringBoard $SCROLL $SCROLL xy,540,900,540,1900:60 wait,1.5 shot,top; home ;;   # no status-bar tap: it opens the App Library's search (keyboard)
    settings) home; STAY=1 $TD/settings-scroll.sh gfx-settings 0 >/dev/null 2>&1 || { log "!! $n: Settings did not open"; home; return; }
              scene $n backboardd,Preferences $SCROLL $TOP shot,top xy,540,1900,540,1100:8 wait,1.5 xy,540,1100,540,1900:8 wait,1.5 up; home ;;
    calendar) home; scene $n backboardd,MobileCal open,calshow:// wait,3 $SCROLL $TOP shot,rest left:30 right:30 up; home ;;
    maps)     home; scene $n backboardd,Maps open,'maps://?ll=37.7749,-122.4194&z=13' wait,5 xy,300,1500,800,900:40 xy,800,900,300,1500:40 \
                xy,540,1600,540,800:40 xy,540,800,540,1600:40 up; home ;;
    web-hn)   home; open_url https://news.ycombinator.com/ 8; scene $n backboardd,MobileSafari,com.apple.WebKit.WebContent,com.apple.WebKit.GPU $WEB $TOP shot,top ;;
    web-wiki) open_url https://en.wikipedia.org/wiki/Main_Page 8; scene $n backboardd,MobileSafari,com.apple.WebKit.WebContent,com.apple.WebKit.GPU $WEB $TOP shot,top ;;
    web-apple) open_url https://www.apple.com/ 10; scene $n backboardd,MobileSafari,com.apple.WebKit.WebContent,com.apple.WebKit.GPU $WEB $TOP shot,top ;;
    web-heavy) open_url ${HEAVY_URL:-https://www.theverge.com/} 12
              scene $n backboardd,MobileSafari,com.apple.WebKit.WebContent,com.apple.WebKit.GPU anim,4 $WEB wait,1 up; home ;;
    *) log "!! unknown scene $s" ;;
  esac
}
for r in $(seq 1 ${REPEAT:-1}); do
  sfx=""; [ $r -gt 1 ] && sfx=-r$r
  for s in $SCENES; do
    e=$(session_end); [ -n "$e" ] && { log "!! session $SESS ended at $(grep -o '[0-9]\{2\}:[0-9]\{2\}:[0-9]\{2\}' <<<"$e" | head -1) (autoreset?): $e -- stopping, scenes from $s$sfx on not measured"; exit 1; }
    log "scene $s$sfx"; run_scene $s "$sfx"
  done
done
home

# 4. Frame traces (one file per Metal process that started with the trace on), stability from the run's log.
mkdir -p $O/frametrace; rm -f $O/frametrace/*.csv
if [ "${TRACE:-1}" = 1 ]; then
  FTD=/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/frametrace
  for f in $(T=20 $SS run $FJB --ls $FTD 2>/dev/null | grep -o "backboardd-${BBPID:-x}\.csv"); do
    T=200 $SS pull $FTD/$f $O/frametrace/$f >/dev/null 2>&1 && log "frame trace $f: $(wc -l < $O/frametrace/$f) lines"
  done
fi
python3 -I - "$LOGF" "$LOG0" $O/stability.json <<'EOF'
import json, re, sys
log, start, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
lines = open(log, errors='replace').read().splitlines()[start:]
crash = [l.split(']:', 1)[-1].strip()[:80] for l in lines if 'CRASH p' in l]
by = {}
for c in crash:
    m = re.search(r'CRASH p\d+ (\S+)', c)
    by[m.group(1) if m else '?'] = by.get(m.group(1) if m else '?', 0) + 1
json.dump({'crashes': sum(by.values()), 'backboardd_crashes': by.get('backboardd', 0),
           'swap_wait_timeouts': sum('swap_wait_gated' in l for l in lines),
           'gpu_faults': sum(bool(re.search(r'SMMU.*fault|hwfault [1-9a-f]|GPU (hang|fault)|fajita-gpu: .*FAIL', l)) for l in lines),
           'panics': sum('panic' in l.lower() for l in lines), 'log_lines': len(lines),
           'crash_by_process': ['%s x%d' % kv for kv in sorted(by.items(), key=lambda kv: -kv[1])]}, open(out, 'w'))
EOF
# The gpu-env from before, unless it held a diagnostic: then the plain one (the trace stays on in this backboardd only)
if [ -f $O/gpu-env.before ] && ! grep -v '^\s*#' $O/gpu-env.before | grep -qE "$DIAGKEYS"; then T=60 $SS push $O/gpu-env.before /private/var/ngpu/gpu-env 644 >/dev/null 2>&1
else printf '# no diagnostics\n' > $O/gpu-env.plain; T=60 $SS push $O/gpu-env.plain /private/var/ngpu/gpu-env 644 >/dev/null 2>&1; fi
if [ "${REBASE:-0}" = 1 ]; then mkdir -p $W/gfx/refs; cp $O/*.png $W/gfx/refs/ 2>/dev/null; log "references saved to $W/gfx/refs"; fi
python3 -I $BOARD/tools/gpu/gfx-report.py $O
[ "${REPEAT:-1}" -gt 1 ] && python3 -I $BOARD/tools/gpu/gfx-report.py --noise $O
exit 0
