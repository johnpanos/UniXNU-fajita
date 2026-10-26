#!/bin/bash
# idle-daemons.sh NAME [ROUNDS] [GAP_S]: background CPU on the held session while it sits idle, one line per round: every process over
# 2% of a core in a 10 s window (ngpu-metal cpu), the busiest non-compositor process, the phone's clock, and the frame state of a Biome
# segment (BIOME=PATH on the phone, default the dasd battery-temperature stream coreduetd spun on; BIOME= to skip; tools/biome/segb.py).
# Default 10 rounds, 180 s apart. Lines also in $FAJITA_WORK/daemons/NAME.txt; the last line is a summary (max and median per daemon of
# coreduetd, dasd, healthappd, healthd, mobileassetd, kbd). Run long watches through bg.sh. NOTES-daemons.md.
#   scripts/fajita/test/bg.sh start idle1 --max 2400 scripts/fajita/test/idle-daemons.sh idle1 12 180
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}; OUTD=${FAJITA_OUT:-$BOARD/.cache/fajita-out}; SS=$TD/session.sh
[ -n "$1" ] || { sed -n 2,7p "$0"; exit 2; }
NAME=$1; ROUNDS=${2:-10}; GAP=${3:-180}
BIOME=${BIOME-/private/var/mobile/Library/Biome/streams/restricted/_DKEvent.Dasd.Batterytemperature/local/0}
D=$W/daemons; mkdir -p $D; out=$D/$NAME.txt
push() { local f=$OUTD/tools/$1 p; [ -f "$f" ] || { echo "no $f (scripts/fajita/build-usbdbg-agent.sh $OUTD/tools/fjdbg-agent)" >&2; exit 1; }
  p=/private/var/ngpu/sbin/$1-$(sha1sum "$f" | cut -c1-8); T=120 $SS push "$f" $p >/dev/null 2>&1 || { echo "push $1 failed" >&2; exit 1; }; echo $p; }
NGPU=$(push ngpu-metal) || exit 1; FJB=$(push fjbundle) || exit 1
for r in $(seq 1 $ROUNDS); do
  t0=$(date +%s)
  cpu=$(T=25 $SS run $NGPU cpu 10 2>&1)
  if ! echo "$cpu" | grep -q "cpu over"; then echo "[$(date +%T)] #$r no answer from the phone: $(echo "$cpu" | tail -1)" | tee -a $out; break; fi
  busy=$(echo "$cpu" | awk '$2+0>2 && $4!="kernel_task"{printf "%s %s  ", $4, $2}')
  clock=$(T=15 $SS run $FJB --find /private/var/ngpu/sbin 0 2>/dev/null | awk '/^now/{print $3}')
  seg=
  if [ -n "$BIOME" ]; then
    T=60 $SS pull "$BIOME" $D/$NAME.seg >/dev/null 2>&1 && seg=$(python3 -I $BOARD/tools/biome/segb.py $D/$NAME.seg 2>&1 | sed -n 2p | sed 's/^ *//; s/, crc failures 0//')
  fi
  echo "[$(date +%T)] #$r clock $clock (host $(date +%s)) | $busy| biome: ${seg:--}" | tee -a $out
  [ $r -lt $ROUNDS ] && sleep $(( GAP - ($(date +%s) - t0) > 0 ? GAP - ($(date +%s) - t0) : 0 ))
done
python3 -I - "$out" <<'EOF' | tee -a $out
import re, statistics, sys
rows = [l for l in open(sys.argv[1]) if re.match(r'\[\d', l) and ' cpu ' not in l and '| biome' in l]
names = ['coreduetd', 'dasd', 'healthappd', 'healthd', 'mobileassetd', 'kbd']
parts = []
for n in names:
    v = [float(m.group(1)) for l in rows for m in [re.search(r'(?:^|\s)%s ([\d.]+)%%' % n, l.split('|')[1])] if m]
    v += [0.0] * (len(rows) - len(v))
    parts.append('%s max %.1f%% median %.1f%%' % (n, max(v) if v else 0, statistics.median(v) if v else 0))
print('summary over %d rounds: %s' % (len(rows), ', '.join(parts)))
EOF
