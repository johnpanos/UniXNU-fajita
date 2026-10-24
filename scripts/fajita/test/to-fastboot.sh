#!/bin/bash
# to-fastboot.sh: get the phone into fastboot, from pmOS (telnet helper) or by waiting; prints the device.
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}
for i in $(seq 1 300); do
  [ -n "$(fastboot devices)" ] && { fastboot devices; exit 0; }
  if timeout 2 bash -c 'echo > /dev/tcp/172.16.42.1/23' 2>/dev/null; then
    B=$(base64 -w0 $W/rb/rb)
    timeout 20 python3 -I $TD/pmsh.py "echo $B | base64 -d > /tmp/rb && chmod +x /tmp/rb" >/dev/null 2>&1
    timeout 10 python3 -I $TD/pmsh.py "/tmp/rb" 2 >/dev/null 2>&1
    sleep 5
  fi
  sleep 2
done
echo "no fastboot device" >&2; exit 1
