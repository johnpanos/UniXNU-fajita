#!/bin/bash
# to-fastboot.sh [MAX_S]: get the phone into fastboot from wherever it is, and print the device. Tries, each turn:
#   - already in fastboot: done;
#   - iOS with the USB debug gadget (1209:4d69): a reset through the hub if one is serving, else directly on the gadget's tty;
#   - pmOS (telnet helper at 172.16.42.1:23): the rb reboot helper.
# Gives up after MAX_S seconds (default 180) with a non-zero exit instead of waiting forever.
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}
MAX=${1:-180}; end=$(( $(date +%s) + MAX )); tried_ios=0
while [ "$(date +%s)" -lt $end ]; do
  [ -n "$(fastboot devices)" ] && { fastboot devices; exit 0; }
  if lsusb -d 1209:4d69 >/dev/null 2>&1 && [ $tried_ios -lt 3 ]; then
    tried_ios=$((tried_ios + 1))
    if pgrep -f "fajita-usbdbg-hub.py serve" >/dev/null; then
      echo "to-fastboot: iOS with the debug gadget: reset through the hub"; T=10 "$TD/hc" reset >/dev/null 2>&1
    else
      tty=$(ls /dev/ttyACM* 2>/dev/null | head -1)
      if [ -n "$tty" ]; then
        echo "to-fastboot: iOS with the debug gadget, no hub: reset on $tty"
        timeout 15 python3 -I "$TD/../../fajita-usbdbg.py" --port "$tty" reset >/dev/null 2>&1
      fi
    fi
    sleep 8; continue
  fi
  if timeout 2 bash -c 'echo > /dev/tcp/172.16.42.1/23' 2>/dev/null; then
    B=$(base64 -w0 $W/rb/rb)
    timeout 20 python3 -I $TD/pmsh.py "echo $B | base64 -d > /tmp/rb && chmod +x /tmp/rb" >/dev/null 2>&1
    timeout 10 python3 -I $TD/pmsh.py "/tmp/rb" 2 >/dev/null 2>&1
    sleep 5
  fi
  sleep 2
done
echo "to-fastboot: no fastboot device after ${MAX}s (gadget: $(lsusb -d 1209:4d69 >/dev/null 2>&1 && echo present || echo absent))" >&2; exit 1
