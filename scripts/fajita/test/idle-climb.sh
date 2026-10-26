#!/bin/bash
# idle-climb.sh [MINUTES] [PROC]: does PROC (default backboardd) use more CPU the longer the phone sits idle? On the held session: the
# screen is locked (power key) and left alone for MINUTES (default 15); every minute PROC's CPU over 5 s is recorded (ngpu-metal cpu,
# its busiest threads), and at the start and the end its threads are sampled (proc-sample.sh, 4 s). Prints the minute-by-minute CPU
# and writes everything to $FAJITA_WORK/idle-climb/: cpu.tsv, sample-start.txt, sample-end.txt. A CPU that climbs with idle time is
# per-frame work that grows (a table never pruned, a log kept in memory): the end sample says where.
#   SYMS=... (proc-sample.sh) names the GPU plugin's frames from the build the phone runs.
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}; OUTD=${FAJITA_OUT:-$BOARD/.cache/fajita-out}; SS=$TD/session.sh
MIN=${1:-15}; P=${2:-backboardd}; O=$W/idle-climb; mkdir -p $O
f=$OUTD/tools/ngpu-metal; [ -f "$f" ] || { echo "no $f (scripts/fajita/build-usbdbg-agent.sh $OUTD/tools/fjdbg-agent)"; exit 1; }
NGPU=/private/var/ngpu/sbin/ngpu-metal-$(sha1sum "$f" | cut -c1-8); T=120 $SS push "$f" $NGPU >/dev/null 2>&1
T=10 $SS call kcmd key power >/dev/null 2>&1; sleep 3   # lock: the lock screen's clock is the only thing that renders
$TD/proc-sample.sh $P 4 50 > $O/sample-start.txt 2>&1
printf 'minute\tpct\tbusiest threads\n' > $O/cpu.tsv
for m in $(seq 1 $MIN); do
  t0=$(date +%s)
  out=$(T=30 $SS run $NGPU cpu 5 $P 2>&1)
  pct=$(echo "$out" | awk -v p=$P '$4==p{print $2; exit}')
  thr=$(echo "$out" | sed -n '/^threads of/,$p' | sed -n 2,4p | awk '{printf "%s %s; ", $2, $4}')
  printf '%d\t%s\t%s\n' $m "${pct:-?}" "$thr" | tee -a $O/cpu.tsv
  sleep $(( 60 - ($(date +%s) - t0) > 0 ? 60 - ($(date +%s) - t0) : 0 ))
done
$TD/proc-sample.sh $P 4 50 > $O/sample-end.txt 2>&1
echo "samples: $O/sample-start.txt $O/sample-end.txt"
