#!/bin/bash
# session.sh: boot iOS once and keep it up, then run tests back to back over USB without rebooting.
#   session.sh start [PAYLOAD [BOOT]]   flash + boot (default: the scripts/fajita/build.sh --test output), wait for userland
#   session.sh run CMD...        run a userland command on the phone (output + exit status)
#   session.sh push LOCAL REMOTE [MODE]  copy a file onto the phone (default mode 755; binaries: push to /private/var/ngpu/sbin/X,
#                                run as /usr/sbin/X), checked by size and CRC32
#   session.sh pull REMOTE LOCAL copy a file off the phone
#   session.sh call OP ARGS...   any hub op: kcmd TEXT, read32 ADDR [N], write32, readp/readv, screenshot FILE, state
#   session.sh log               follow the kernel log
#   session.sh end NAME          reset the phone now, only if NAME is the cycle holding the phone (your own session)
# The session is an ordinary cycle.sh run (phone.lock held, hub, USB log in logs/NAME/usb-console.txt), so the autoreset and
# wdstop backstops still end it on their own if nobody calls `end`, and it ends itself after SESSION_IDLE (300) s without a
# run/push/pull/call, and after SESSION_MAX (720) s when another run is queued on phone.lock. Kernel changes still need a new session.
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; S=$W
HUB="python3 -I $TD/../../fajita-usbdbg-hub.py"
CUR=$S/session.current
ACT=${XDG_RUNTIME_DIR:-/tmp}/fjdbg-hub.sock.activity   # stamped by every hub call (any script); an idle session ends itself (SESSION_IDLE s, default 300)
case $1 in run|push|pull|call) touch $ACT;; esac
case $1 in
start)
  O=${FAJITA_OUT:-$TD/../../../.cache/fajita-out}; P=${2:-$O/payload.img}; B=${3:-$O/boot.img}; [ -f "$P" ] || { echo "no payload $P"; exit 1; }
  NAME=session-$(date +%H%M%S); echo $NAME > $CUR
  CYCLE_WAIT=${CYCLE_WAIT:-2000} nohup $TD/cycle.sh $NAME $P $B > $S/logs-$NAME.out 2>&1 &
  echo "$NAME: waiting for the phone lock and boot (log: $S/logs/$NAME/)"
  # Fail fast: the run ended (RESULT) or the phone fell back to fastboot before the kernel logged "XNU booting" = a dead boot.
  until grep -q "XNU booting" $S/logs/$NAME/cycle.txt 2>/dev/null; do
    if grep -qE "^\[[0-9:]+\] RESULT:|back in fastboot" $S/logs/$NAME/cycle.txt 2>/dev/null; then
      echo "FAIL boot: $NAME ended before the kernel came up (see $S/logs/$NAME/cycle.txt)"; exit 3
    fi
    sleep 2
  done
  t0=$(date +%s)
  touch $ACT
  # idle guard: nobody should hold the phone without using it; reset the run after SESSION_IDLE s with no command
  ( while ps -eo args | grep -q "^/bin/bash .*cycle.sh $NAME "; do
      ps -eo args | grep -v grep | grep -q "fajita-usbdbg-hub.py call" && touch $ACT   # a long call in flight is activity
      [ $(( $(date +%s) - $(stat -c %Y $ACT) )) -gt ${SESSION_IDLE:-300} ] && { echo "[$(date +%T)] idle ${SESSION_IDLE:-300} s: ending $NAME" >> $S/logs/$NAME/cycle.txt; $HUB call reset >/dev/null 2>&1; break; }
      # fair share: past SESSION_MAX s (default 720) while another run waits on phone.lock, the session ends
      if [ $(( $(date +%s) - t0 )) -gt ${SESSION_MAX:-720} ] && ps -eo args | grep "flock.*phone.lock" | grep -v grep | grep -vq "cycle.sh $NAME "; then
        echo "[$(date +%T)] held ${SESSION_MAX:-720} s with others queued: ending $NAME" >> $S/logs/$NAME/cycle.txt; $HUB call reset >/dev/null 2>&1; break
      fi
      sleep 10
    done ) > /dev/null 2>&1 &
  for i in $(seq 1 90); do
    # the phone fell back to fastboot (panic or reset) with the session still waiting: stop now
    if grep -qE "back in fastboot|^\[[0-9:]+\] RESULT:" $S/logs/$NAME/cycle.txt 2>/dev/null; then echo "FAIL boot: phone back in fastboot (panic or reset?) after $(( $(date +%s) - t0 )) s"; exit 3; fi
    $HUB call --timeout 20 run /usr/sbin/ioreg -d 1 2>/dev/null | grep -q "+-o" && { echo "ready after $(( $(date +%s) - t0 )) s"; exit 0; }
    sleep 3
  done
  echo "userland agent did not answer; kernel ops (kcmd, read32, state) may still work"; exit 1;;
run)  shift; exec $HUB call --timeout ${T:-60} run "$@";;
push) shift; exec $HUB call --timeout ${T:-300} push "$(realpath "$1")" "${@:2}";;
pull) shift; exec $HUB call --timeout ${T:-300} pull "$1" "$(realpath -m "$2")";;
call) shift; exec $HUB call "$@";;
log)  exec tail -f $S/logs/$(cat $CUR)/usb-console.txt;;
end)  # only the owner ends a session: pass its name, and it must be the run that holds the phone now
      [ -n "$2" ] || { echo "usage: session.sh end NAME (the session you started)"; exit 1; }
      cur=$(ps -eo args | grep "^/bin/bash .*cycle.sh " | grep -o "cycle.sh [^ ]*" | head -1 | cut -d" " -f2)   # the running cycle, not ones waiting on the lock
      [ "$cur" = "$2" ] || { echo "refusing: the running cycle is '$cur', not '$2'"; exit 1; }
      $HUB call reset; echo "ending $2";;
*) sed -n 2,9p "$0"; exit 1;;
esac
