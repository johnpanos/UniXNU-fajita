#!/bin/bash
# cycle.sh NAME [PAYLOAD BOOTIMG]: one unattended test run. From fastboot: battery check (logged only: iOS charges), flash the
# payload to system_a, boot XNU, wait for its auto-reset or panic reset to land in fastboot again, then boot the pmOS debug shell, pull and
# decode the UniXNU log window (pull-log.sh) and reboot to fastboot. Output: logs/NAME/. Prints one RESULT line.
# Default: the kernel log streams over USB (fjdbg hub) and the ~5 min pmOS pull only runs when that capture failed.
# fastboot runs in its own session: a kill of this script's process group (bg.sh kill, --max) then never cuts a fastboot
# exchange in half, which wedged the bootloader twice (no command answered until a hardware reset). Bounded by a timeout.
fb() { setsid -w timeout ${FB_TIMEOUT:-120} fastboot "$@"; }
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; S=$W
# One run on the phone at a time (several agents use it): the whole cycle holds $W/phone.lock.
# The phone lock is global (one phone, any worktree or work dir): ${XDG_RUNTIME_DIR:-/tmp}/fajita-phone.lock. The old per-work-dir lock
# ($S/phone.lock) is taken too, so copies of this script from before the change still exclude this one.
G=${XDG_RUNTIME_DIR:-/tmp}/fajita-phone.lock
[ -n "$FAJITA_PHONE_LOCKED" ] || { export FAJITA_PHONE_LOCKED=1; exec flock "$G" flock $S/phone.lock "$0" "$@"; }
O=${FAJITA_OUT:-$TD/../../../.cache/fajita-out}
NAME=$1; PAYLOAD=${2:-$O/payload.img}; BOOT=${3:-$O/boot.img}   # default: the scripts/fajita/build.sh output
OUT=$S/logs/$NAME; mkdir -p $OUT
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a $OUT/cycle.txt; }
wait_fastboot() { for i in $(seq 1 $1); do [ -n "$(fb devices)" ] && return 0; sleep 2; done; return 1; }
arm() { for i in 1 2 3 4 5; do fb set_active b >/dev/null 2>&1; [ "$(fb getvar slot-unbootable:b 2>&1 | head -1)" = "slot-unbootable:b: no" ] && return 0; sleep 2; done; return 1; }
volt() { fb getvar battery-voltage 2>&1 | head -1 | grep -o '[0-9]*$'; }
wait_fastboot 30 || { log "RESULT: no fastboot at start"; exit 1; }
v=$(volt); log "battery ${v} mV"
# Rule (the user's mandate): no pmOS unless iOS cannot do the job. XNU charges the battery itself (fajita-pmic=2 in board_args),
# so a low battery is only logged; the run charges at ~1 A from ~30 s after boot.
[ -n "$v" ] && [ "$v" -lt 3650 ] && log "battery low (${v} mV): the iOS run charges it"
fb flash system_a $PAYLOAD 2>&1 | tail -1 | tee -a $OUT/cycle.txt
arm; fb boot $BOOT 2>&1 | tail -1 | tee -a $OUT/cycle.txt
t0=$(date +%s)
for i in $(seq 1 60); do [ -z "$(fb devices)" ] && break; sleep 1; done
log "XNU booting"
# USB log streaming (default): the fjdbg hub (scripts/fajita-usbdbg-hub.py) owns the gadget's tty for this run only, appends the
# kernel log to usb-console.txt and serves `fajita-usbdbg-hub.py call run CMD` / read32 / kcmd ... to other scripts on its socket
# (default $XDG_RUNTIME_DIR/fjdbg-hub.sock). Never open the tty directly while a cycle runs. CYCLE_HUB=0 turns it off; callers that
# still drive the tty themselves set CYCLE_NOPULL explicitly, which also turns it off unless CYCLE_HUB=1.
HUB=${CYCLE_HUB_SCRIPT:-$TD/../../fajita-usbdbg-hub.py}; usbpid=; shotpid=
[ -n "$CYCLE_HUB" ] || { [ -n "$CYCLE_NOPULL" ] && CYCLE_HUB=0 || CYCLE_HUB=1; }
if [ "$CYCLE_HUB" = 1 ]; then
  if lsusb -d 1209:4d69 >/dev/null 2>&1; then log "a fjdbg gadget is already present: not attaching the hub"
  else python3 -I $HUB serve --owner-pid $$ --log $OUT/usb-console.txt > $OUT/hub.txt 2>&1 & usbpid=$!; log "usb hub pid $usbpid"; fi
fi
# CYCLE_SHOTS="SECONDS[,SECONDS...]": screenshots over USB through the hub at those times after boot (counted from "XNU booting"),
# saved as logs/NAME/shot-<SECONDS>.png (half resolution; the hub's `call screenshot FILE`); results in shots.txt. No pmOS needed.
if [ -n "$CYCLE_SHOTS" ] && [ -n "$usbpid" ]; then
  ( for s in $(echo "$CYCLE_SHOTS" | tr ',' ' '); do
      while [ $(( $(date +%s) - t0 )) -lt "$s" ]; do sleep 1; done
      python3 -I $HUB call wait-up 60 >/dev/null 2>&1
      python3 -I $HUB call --timeout 90 screenshot $OUT/shot-$s.png >> $OUT/shots.txt 2>&1 || echo "shot at ${s}s failed" >> $OUT/shots.txt
    done ) & shotpid=$!
  log "screenshots at ${CYCLE_SHOTS} s (pid $shotpid)"
fi
# fair-share guard for every run (scripts that bypass session.sh too): while another run waits on phone.lock, a run that has made no
# hub call for CYCLE_IDLE s (default 300), or has held the phone over CYCLE_MAX s (default 900), is reset so the queue moves.
if [ -n "$usbpid" ] && [ "${CYCLE_GUARD:-1}" = 1 ]; then
  ( ACT=${XDG_RUNTIME_DIR:-/tmp}/fjdbg-hub.sock.activity; g0=$(date +%s); touch $ACT
    while kill -0 $usbpid 2>/dev/null; do
      sleep 15
      ps -eo args | grep "flock.*phone.lock" | grep -v grep | grep -vq "cycle.sh $NAME " || continue   # nobody waiting
      # a hub call still in flight (a long pull or push) counts as activity
      ps -eo args | grep -v grep | grep -q "fajita-usbdbg-hub.py call" && touch $ACT
      idle=$(( $(date +%s) - $(stat -c %Y $ACT) )); held=$(( $(date +%s) - g0 ))
      if [ $idle -gt ${CYCLE_IDLE:-300} ] || [ $held -gt ${CYCLE_MAX:-1200} ]; then
        echo "[$(date +%T)] guard: idle ${idle}s / held ${held}s with others queued: resetting $NAME" >> $OUT/cycle.txt
        python3 -I $HUB call reset >/dev/null 2>&1; break
      fi
    done ) > /dev/null 2>&1 &
fi
stop_hub() { [ -n "$shotpid" ] && { kill $shotpid; } 2>/dev/null; shotpid=; [ -n "$usbpid" ] && { kill $usbpid; sleep 1; kill -9 $usbpid; } 2>/dev/null; usbpid=; }
trap stop_hub EXIT
# Back in fastboot, or stuck in Qualcomm CrashDump Mode (05c6:900e): answer Sahara with RESET (sahara-reset.py) and keep waiting.
back=0
for i in $(seq 1 $(( ${CYCLE_WAIT:-900} / 2 ))); do   # CYCLE_WAIT: seconds to wait for the run (2 s per turn)
  [ -n "$(fb devices)" ] && { back=1; break; }
  if lsusb | grep -q "05c6:900e\|05c6:9008"; then
    log "crashdump mode seen: sending Sahara RESET"
    python3 -I $TD/sahara-reset.py 2>&1 | tee -a $OUT/cycle.txt
  fi
  sleep 2
done
[ $back = 1 ] || stop_hub
[ $back = 1 ] || { log "RESULT: phone did not come back within ${CYCLE_WAIT:-900} s (hard hang?)"; exit 2; }
log "back in fastboot after $(( $(date +%s) - t0 ))s"
[ -n "$usbpid" ] && { stop_hub; log "usb console: $(wc -l < $OUT/usb-console.txt 2>/dev/null || echo 0) lines in usb-console.txt"; }
[ -n "$CYCLE_SHOTS" ] && log "screenshots: $(ls $OUT/shot-*.png 2>/dev/null | wc -l) of $(echo "$CYCLE_SHOTS" | tr ',' ' ' | wc -w) ($OUT/shot-*.png)"
# Stale-branch guard: ios16-port charges by default (fajita-pmic=2). A run whose log has no "fajita-pmic: [pmic] stage 2" row was built
# from a branch without it (or with charging turned off): merge the latest ios16-port before the next build.
if [ -s $OUT/usb-console.txt ]; then
  grep -aq "fajita-pmic: \[pmic\] stage 2" $OUT/usb-console.txt && log "charging: on" || log "WARNING: no charging in this run (no fajita-pmic stage 2 row): merge the latest ios16-port before building"
  grep -aq "fajita-touch: FT5\|fajita-touch: TCH" $OUT/usb-console.txt && log "touch: on" || log "WARNING: no touch in this run (no fajita-touch FT5/TCH row): build from the latest ios16-port (fajita-touch=4 in board_args)"
fi
# pmOS log pull only when iOS could not deliver: the USB capture has 50 lines or fewer. CYCLE_PMOS="<reason>" forces one for something
# only the UFS log has (logged with its reason); CYCLE_NOPULL=0 alone no longer forces a pull.
if [ -z "$CYCLE_PMOS" ] && [ "${CYCLE_NOPULL:-auto}" != 1 ]; then
  [ "${CYCLE_NOPULL:-auto}" = 0 ] && log "CYCLE_NOPULL=0 ignored: set CYCLE_PMOS=<reason> if pmOS is really needed"
  [ "$(wc -l < $OUT/usb-console.txt 2>/dev/null || echo 0)" -gt 50 ] && { log "RESULT: done, USB log in $OUT/usb-console.txt (no pmOS)"; exit 0; }
  log "USB capture missing or short: falling back to the pmOS log pull"
fi
[ -n "$CYCLE_PMOS" ] && log "pmOS pull requested: $CYCLE_PMOS"
# CYCLE_NOPULL=1: a probe run that stops before the kernel writes its log; skip the pmOS log pull.
[ "${CYCLE_NOPULL:-0}" = 1 ] && { log "RESULT: done (no log pull)"; exit 0; }
arm; fb boot $S/pmos-debug-shell.img 2>&1 | tail -1 >> $OUT/cycle.txt
for i in $(seq 1 60); do timeout 2 bash -c 'echo > /dev/tcp/172.16.42.1/23' 2>/dev/null && break; sleep 2; done
sleep 3
$TD/pull-log.sh $OUT 2>&1 | tee -a $OUT/cycle.txt
B=$(base64 -w0 $S/rb/rb)
timeout 20 python3 -I $TD/pmsh.py "echo $B | base64 -d > /tmp/rb && chmod +x /tmp/rb" >/dev/null 2>&1
timeout 10 python3 -I $TD/pmsh.py "/tmp/rb" 2 >/dev/null 2>&1
# The reboot helper can hang in D state; sysrq 'b' over telnet reboots pmOS to fastboot within seconds.
wait_fastboot 45 || { log "rb did not return: sysrq reboot"; timeout 10 python3 -I $TD/pmsh.py "echo b > /proc/sysrq-trigger" >/dev/null 2>&1; }
wait_fastboot 60 && log "RESULT: done, log in $OUT" || log "RESULT: log pulled but reader did not return to fastboot"
