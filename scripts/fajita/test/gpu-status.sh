#!/bin/bash
# gpu-status.sh [NAME]: one-shot health of the run holding the phone (or NAME), from its streamed USB log and the host side.
# Prints: phone holder and queue, log freshness, GPU stages, panics, crash counts per process, governor/temperature, touch and
# charging rows, hub activity age. Exit 0 healthy, 1 problem found (panic, stale log, backboardd crash loop, missing stage), 2 no run.
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; S=$W
run=${1:-$(ps -eo args | grep "^/bin/bash .*cycle.sh " | grep -o "cycle.sh [^ ]*" | head -1 | cut -d' ' -f2)}
queue=$(ps -eo args | grep "flock.*phone.lock" | grep -v grep | grep -o "cycle.sh [^ ]*" | cut -d' ' -f2 | grep -vx "$run" | tr '\n' ' ')
[ -n "$run" ] || { echo "no run holds the phone; queued: ${queue:-none}; fastboot: $(fastboot devices | cut -f1)"; exit 2; }
L=$S/logs/$run/usb-console.txt; C=$S/logs/$run/cycle.txt; bad=0
echo "run $run   queued: ${queue:-none}"
grep -a "XNU booting\|back in fastboot\|RESULT\|idle" $C 2>/dev/null | tail -2 | sed 's/^/  cycle: /'
if [ -s "$L" ]; then
  age=$(( $(date +%s) - $(stat -c %Y $L) )); up=$(grep -ao '^\[ *[0-9]*\.' $L | tail -1 | tr -d '[ .')
  echo "  log: $(wc -l < $L) lines, uptime ${up:-?} s, last line ${age} s ago"; [ $age -gt 60 ] && { echo "  !! log stale (${age} s): phone hung, reset, or hub gone"; bad=1; }
else echo "  !! no usb-console.txt yet"; bad=1; fi
lsusb -d 1209:4d69 >/dev/null 2>&1 && echo "  gadget: present" || { echo "  !! gadget absent"; bad=1; }
st=$(grep -ao "fajita-gpu: stage [0-9]* passed" $L 2>/dev/null | tail -1 | grep -o "stage [0-9]*")
fail=$(grep -a "fajita-gpu: stage [0-9]* failed\|fajita-gpu: .*FAIL" $L 2>/dev/null | tail -1 | cut -c1-120)
echo "  gpu: last ${st:-no stage passed}${fail:+ | $fail}"; [ -n "$fail" ] && bad=1
p=$(grep -a "\*\*\* PANIC\|panic(" $L 2>/dev/null | head -2); [ -n "$p" ] && { echo "  !! $p" | cut -c1-200; bad=1; }
echo "  crashes: $(grep -ao 'CRASH p[0-9]* [^ ]*' $L 2>/dev/null | awk '{print $3}' | sort | uniq -c | sort -rn | head -5 | awk '{printf "%s x%s  ", $2, $1}')"
bb=$(grep -ac "CRASH p[0-9]* backboardd" $L 2>/dev/null); [ "${bb:-0}" -ge 3 ] && { echo "  !! backboardd crashed $bb times"; bad=1; }
grep -a "fajita-gpu: gov" $L 2>/dev/null | tail -1 | sed 's/^.*gov: /  gov: /' | cut -c1-110
echo "  touch rows $(grep -ac 'fajita-touch: FT5\|fajita-touch: TCH' $L 2>/dev/null), charging rows $(grep -ac 'pmic\] stage 2' $L 2>/dev/null)"
A=${XDG_RUNTIME_DIR:-/tmp}/fjdbg-hub.sock.activity; [ -f $A ] && echo "  hub: last call $(( $(date +%s) - $(stat -c %Y $A) )) s ago"
exit $bad
