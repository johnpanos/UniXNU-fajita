#!/bin/bash
# bg.sh: run long jobs (builds, fps.sh, session start, leakwatch, pulls) without ever blocking a tool call on them.
#   bg.sh start NAME [--max S] [--quiet] CMD...   start CMD in the background (output in $W/jobs/NAME.log), return at once; killed after
#                                       --max seconds (default 1800). NAME must be new (or finished). --quiet: a job that is silent by
#                                       design (a phone session held open) is never flagged STALLED.
#   bg.sh status [NAME]                 one line per job: running/done/killed/STALLED, elapsed, exit code, output age; with NAME also
#                                       its last 8 output lines. STALLED = running but no new output for BG_STALL s (default 180).
#   bg.sh wait NAME [S]                 wait up to S seconds (default 60, capped at 90) for NAME to finish, print its last 20 lines, then
#                                       return (exit 0 done ok, 1 done failed, 2 still running, 3 stalled), so the caller polls again.
#   bg.sh kill NAME                     stop it (and its children).
# A finished job's exit code is in $W/jobs/NAME.rc. State lives in .cache/fajita-work/jobs ($FAJITA_WORK).
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; J=$W/jobs; mkdir -p "$J"
STALL=${BG_STALL:-180}
now() { date +%s; }
state() {   # state NAME -> running|done|killed|STALLED ...
  local n=$1 pid rc
  pid=$(cat "$J/$n.pid" 2>/dev/null)
  if [ -f "$J/$n.rc" ]; then rc=$(cat "$J/$n.rc"); case $rc in killed|timeout) echo "$rc";; *) echo "done rc=$rc";; esac; return; fi
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    local age=$(( $(now) - $(stat -c %Y "$J/$n.log") ))
    if [ -f "$J/$n.quiet" ] || [ $age -le "$STALL" ]; then echo running; else echo "STALLED (no output ${age}s)"; fi
  else echo "gone (no rc: killed from outside?)"; fi
}
line() { local n=$1 t0; t0=$(cat "$J/$n.t0" 2>/dev/null || now); printf '%-24s %-28s %5ss  output %ss ago\n' "$n" "$(state "$n")" $(( $(now) - t0 )) $(( $(now) - $(stat -c %Y "$J/$n.log" 2>/dev/null || now) )); }
case $1 in
start)
  n=$2; shift 2; max=1800; quiet=0
  while :; do case $1 in --max) max=$2; shift 2;; --quiet) quiet=1; shift;; *) break;; esac; done
  [ -n "$n" ] && [ $# -gt 0 ] || { sed -n 2,11p "$0"; exit 1; }
  case $(state "$n") in running*|STALLED*) echo "$n is still running (bg.sh status $n)"; exit 1;; esac
  rm -f "$J/$n.rc" "$J/$n.pid" "$J/$n.quiet"; : > "$J/$n.log"; now > "$J/$n.t0"; [ $quiet = 1 ] && : > "$J/$n.quiet"
  # the job runs in its own process group (its pid = the group id), so kill and --max take its whole tree; the wrapper outlives it
  # and records the exit code, or "timeout"
  bash -c '
    J=$1; n=$2; max=$3; shift 3
    setsid "$@" >> "$J/$n.log" 2>&1 < /dev/null & c=$!
    echo $c > "$J/$n.pid"
    ( sleep "$max"; [ -f "$J/$n.rc" ] || { echo timeout > "$J/$n.rc"; echo "== bg.sh: killed after ${max}s" >> "$J/$n.log"; kill -TERM -- -$c 2>/dev/null; sleep 5; kill -KILL -- -$c 2>/dev/null; } ) > /dev/null 2>&1 & w=$!
    wait $c; rc=$?; kill $w 2>/dev/null
    [ -f "$J/$n.rc" ] || echo $rc > "$J/$n.rc"' bgjob "$J" "$n" "$max" "$@" > /dev/null 2>&1 &
  for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$J/$n.pid" ] && break; sleep 0.1; done
  echo "started $n (pid $(cat "$J/$n.pid"), max ${max}s): bg.sh status $n / bg.sh wait $n";;
status)
  if [ -n "$2" ]; then line "$2"; tail -8 "$J/$2.log" 2>/dev/null | sed 's/^/  | /'
  else for f in $(ls -t "$J"/*.log 2>/dev/null | head -12); do line "$(basename "$f" .log)"; done; fi;;
wait)
  n=$2; lim=${3:-60}; [ "$lim" -gt 90 ] && lim=90; end=$(( $(now) + lim ))
  while [ $(now) -lt $end ]; do
    s=$(state "$n"); case $s in running) sleep 3;; *) break;; esac
  done
  tail -20 "$J/$n.log" 2>/dev/null
  s=$(state "$n"); echo "== $n: $s"
  case $s in "done rc=0") exit 0;; done*|killed|timeout|gone*) exit 1;; STALLED*) exit 3;; *) exit 2;; esac;;
kill)
  pid=$(cat "$J/$2.pid" 2>/dev/null); echo killed > "$J/$2.rc"; [ -n "$pid" ] && { kill -TERM -- -"$pid" 2>/dev/null; sleep 2; kill -KILL -- -"$pid" 2>/dev/null; }; echo "killed $2";;
*) sed -n 2,11p "$0"; exit 1;;
esac
