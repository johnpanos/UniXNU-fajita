#!/bin/bash
# net-test.sh: the fajita USB network transport (fjdbg NET channel 7, usb-net=1; NOTES-wifi.md "Round 8").
# Proves frames round-trip phone <-> laptop <-> passt without any iOS network interface. Stages:
#
#   sim     host only, no phone: the device-side NET core (UsbNet.h) in tests/UsbDbgSim behind a private hub (--exec),
#           the echo test, then usbnet-host.py bridging to a real passt and the in-kernel probe (DHCP, ARP, ping)
#   ios     on the phone: --test usb-net=1 fajita-net=usb; the BSD interface en0 (package/FajitaNet.cpp) over the NET channel; iOS
#           runs DHCP (ipconfig set en0 DHCP) and pings the gateway through the passt bridge on this laptop
#   usbnet  on the phone: a --test build with usb-net=1 (unless NET_NO_BUILD=1 and the image exists), one held session,
#           `kcmd usbnet` reports the channel up; usbnet-host.py echo (N frames of 14..1536 bytes back intact, in order);
#           then usbnet-host.py bridge to passt and `kcmd usbnet dhcp`: the kernel's DHCP DISCOVER reaches passt, the
#           OFFER/ACK come back, then ARP and an ICMP echo to the router (the probe's rows are in the kernel log)
#
#   scripts/fajita/test/net-test.sh sim
#   scripts/fajita/test/bg.sh start net --max 1500 scripts/fajita/test/net-test.sh usbnet
# Exit status 0 when every requested stage passed. Results in .cache/fajita-work/logs/net-test/report.txt.
set -uo pipefail
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}
OUT=${FAJITA_OUT:-$ROOT/.cache/fajita-out-net}
HUBPY="$ROOT/scripts/fajita-usbdbg-hub.py"
NETPY="$TD/usbnet-host.py"
COUNT=${NET_ECHO_COUNT:-300}
RES=$W/logs/net-test; mkdir -p "$RES"; REPORT=$RES/report.txt; : > "$REPORT"
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$REPORT"; }
fails=0; declare -A VERDICT
verdict() { local s=$1 ok=$2; shift 2; VERDICT[$s]=$ok; if [ "$ok" = PASS ]; then say "PASS $s: $*"; else say "FAIL $s: $*"; fails=$((fails+1)); fi; }

want=("$@"); [ ${#want[@]} -eq 0 ] && want=(sim)
for s in "${want[@]}"; do case $s in sim|usbnet|ios) ;; *) echo "unknown stage '$s'; known: sim usbnet ios"; exit 2;; esac; done
command -v passt >/dev/null || { echo "passt is not installed (dnf install passt)"; exit 2; }

# The echo test, then the passt bridge with the kernel probe, against the hub on socket $1. Sets ok=0 on a failure.
transport() {
  local sock=$1 tag=$2 o b
  local hub=(python3 -I "$HUBPY" --sock "$sock" call --timeout 20)
  o=$("${hub[@]}" kcmd usbnet 2>&1); say "  kcmd usbnet: $o"
  echo "$o" | grep -q 'link=1' || { ok=0; say "  NET channel not up (usb-net=1 missing, or no USB configuration)"; return; }
  o=$(timeout 90 python3 -I "$NETPY" --sock "$sock" echo --count "$COUNT" 2>&1); say "  $o"
  echo "$o" | grep -q 'usbnet echo PASS' || ok=0
  python3 -I "$NETPY" --sock "$sock" bridge -v --seconds 40 --pcap "$RES/$tag.pcap" > "$RES/$tag-bridge.txt" 2>&1 &
  b=$!
  for _ in $(seq 1 50); do grep -q 'usbnet bridge: phone' "$RES/$tag-bridge.txt" 2>/dev/null && break; sleep 0.2; done
  o=$("${hub[@]}" kcmd usbnet dhcp 2>&1); say "  kcmd usbnet dhcp: $o"
  for _ in $(seq 1 40); do
    o=$("${hub[@]}" kcmd usbnet show 2>&1)
    echo "$o" | grep -qE 'state (done|failed)' && break; sleep 0.5
  done
  echo "$o" | while read -r r; do say "  $r"; done
  echo "$o" | grep -q 'probe PASS' || ok=0
  kill -TERM "$b" 2>/dev/null; wait "$b" 2>/dev/null
  grep -E 'dhcp|icmp|arp|end' "$RES/$tag-bridge.txt" | head -14 | while read -r r; do say "  bridge: $r"; done
  say "  hub: $("${hub[@]}" net-stats 2>&1)"
}

stage_sim() {
  local ok=1 sim=$W/usbnet-sim/UsbDbgSim sock=$W/usbnet-sim/hub.sock hubpid
  mkdir -p "$W/usbnet-sim"
  c++ -std=c++17 -Wall -Wextra -Werror -O2 -o "$sim" "$ROOT/drivers/NativeDwc3/tests/UsbDbgSim.cpp" || { verdict sim FAIL "UsbDbgSim did not build"; return; }
  python3 -I "$HUBPY" --sock "$sock" serve --log "$RES/sim-hub.log" --exec "$sim" & hubpid=$!
  for _ in $(seq 1 50); do [ -S "$sock" ] && break; sleep 0.1; done
  transport "$sock" sim
  kill "$hubpid" 2>/dev/null; wait "$hubpid" 2>/dev/null
  if [ "$ok" = 1 ]; then verdict sim PASS "UsbDbgSim <-> hub <-> passt: echo and DHCP/ARP/ping"
  else verdict sim FAIL "see above and $RES"; fi
}

NAME=
session_end() { [ -n "$NAME" ] && "$TD/session.sh" end "$NAME" >/dev/null 2>&1; NAME=; }
trap session_end EXIT

stage_usbnet() {
  local ok=1 con
  if [ -n "${NET_NO_BUILD:-}" ] && [ -f "$OUT/payload.img" ]; then say "build: reusing $OUT"
  else
    say "build: --test usb-net=1 -> $OUT"
    git -C "$ROOT" pull --no-rebase origin ios16-port >/dev/null 2>&1 || true
    FAJITA_OUT=$OUT "$TD/../build.sh" --test usb-net=1 2>&1 | tee "$RES/build.txt" \
      || { verdict usbnet FAIL "build failed (see $RES/build.txt)"; tail -6 "$RES/build.txt" | tee -a "$REPORT"; return; }
  fi
  local out; out=$(FAJITA_OUT=$OUT "$TD/session.sh" start 2>&1) || { verdict usbnet FAIL "session start failed"; return; }
  NAME=$(grep -om1 'session-[0-9]\{6\}' <<<"$out" | head -1); con=$W/logs/$NAME/usb-console.txt   # own session, not the shared session.current
  say "session $NAME (console $con)"
  transport "${FAJITA_USBDBG_HUB:-${XDG_RUNTIME_DIR:-/tmp}/fjdbg-hub.sock}" usbnet
  grep -a 'usbnet: ' "$con" 2>/dev/null | tail -8 | while read -r r; do say "  log: $r"; done
  grep -aq 'usbnet: probe PASS' "$con" 2>/dev/null || { ok=0; say "  the probe's PASS row is not in the kernel log"; }
  session_end
  if [ "$ok" = 1 ]; then verdict usbnet PASS "phone <-> hub <-> passt: echo and the kernel's DHCP/ARP/ping"
  else verdict usbnet FAIL "see above and $RES"; fi
}

# The iOS network interface (package/FajitaNet.cpp, fajita-net=usb): the BSD interface en0 over the NET channel. DHCP and ping through passt on this laptop,
# with package/usb-debug-tools/fjnet.c (iOS ships no ifconfig/ipconfig/ping).
stage_ios() {
  local ok=1 con b o ip gw
  if [ -n "${NET_NO_BUILD:-}" ] && [ -f "$OUT/payload.img" ]; then say "build: reusing $OUT"
  else
    say "build: --test usb-net=1 fajita-net=usb -> $OUT"
    git -C "$ROOT" pull --no-rebase origin ios16-port >/dev/null 2>&1 || true
    FAJITA_OUT=$OUT "$TD/../build.sh" --test usb-net=1 fajita-net=usb 2>&1 | tee "$RES/build.txt" \
      || { verdict ios FAIL "build failed (see $RES/build.txt)"; tail -6 "$RES/build.txt" | tee -a "$REPORT"; return; }
  fi
  local out; out=$(FAJITA_OUT=$OUT "$TD/session.sh" start 2>&1) || { verdict ios FAIL "session start failed"; return; }
  NAME=$(grep -om1 'session-[0-9]\{6\}' <<<"$out" | head -1); con=$W/logs/$NAME/usb-console.txt   # own session, not the shared session.current
  say "session $NAME (console $con)"
  local sock=${FAJITA_USBDBG_HUB:-${XDG_RUNTIME_DIR:-/tmp}/fjdbg-hub.sock}
  python3 -I "$NETPY" --sock "$sock" bridge -v --seconds 150 --pcap "$RES/ios.pcap" > "$RES/ios-bridge.txt" 2>&1 &
  b=$!
  for _ in $(seq 1 50); do grep -q 'usbnet bridge: phone' "$RES/ios-bridge.txt" 2>/dev/null && break; sleep 0.2; done
  grep -a 'fajita-net' "$con" 2>/dev/null | tail -3 | while read -r r; do say "  log: $r"; done
  grep -aq 'fajita-net: en0 attached' "$con" 2>/dev/null || { ok=0; say "  en0 was not attached (fajita-net: rows above)"; }
  local FJN=/private/var/ngpu/sbin/fjnet
  [ -x "$ROOT/.cache/fajita-inputs/fjnet" ] || "$TD/../build-usbdbg-agent.sh" "$ROOT/.cache/fajita-inputs/fjdbg-agent" >/dev/null 2>&1
  "$TD/session.sh" push "$ROOT/.cache/fajita-inputs/fjnet" $FJN >/dev/null 2>&1 || { ok=0; say "  push fjnet failed"; }
  o=$("$TD/session.sh" run $FJN show en0 2>&1); say "  show en0: $(echo "$o" | tr '\n' ' ')"
  echo "$o" | grep -q 'link' || ok=0
  # does iOS itself (configd/IPConfiguration) pick the bare BSD interface up? Look before our own DHCP runs.
  sleep 12
  say "  configd before fjnet dhcp: $("$TD/session.sh" run $FJN show en0 2>&1 | grep -c 'inet ') inet address(es) on en0"
  "$TD/session.sh" run $FJN sc 'State:/Network/.*' > "$RES/ios-sc-before.txt" 2>&1
  say "  SCDynamicStore State:/Network keys before: $(head -1 "$RES/ios-sc-before.txt"); en0 keys: $(grep -c 'en0' "$RES/ios-sc-before.txt")"
  o=$("$TD/session.sh" run $FJN dhcp en0 2>&1); say "  dhcp: $(echo "$o" | tr '\n' ' ')"
  ip=$(echo "$o" | sed -n 's/^lease \([0-9.]*\) gw .*/\1/p'); gw=$(echo "$o" | sed -n 's/^lease .* gw \([0-9.]*\)$/\1/p')
  [ -n "$ip" ] || ok=0
  if [ -n "${gw:-}" ] && [ "$gw" != 0.0.0.0 ]; then
    o=$("$TD/session.sh" run $FJN ping "$gw" 3 2>&1); say "  ping $gw: $(echo "$o" | tail -2 | tr '\n' ' ')"
    echo "$o" | grep -q '^3/3 received' || ok=0
  else ok=0; fi
  sleep 5
  "$TD/session.sh" run $FJN sc 'State:/Network/.*' > "$RES/ios-sc-after.txt" 2>&1
  say "  SCDynamicStore after our lease: $(head -1 "$RES/ios-sc-after.txt"); en0 keys: $(grep -c 'en0' "$RES/ios-sc-after.txt"); IPv4 global: $(grep 'Global/IPv4 ' "$RES/ios-sc-after.txt" | cut -c1-200)"
  if [ -n "${gw:-}" ]; then   # an app-level fetch through CFNetwork: a local server on this laptop (passt maps the gateway to the host) and the internet
    ( cd "$RES" && exec python3 -I -m http.server 8099 --bind 0.0.0.0 ) >/dev/null 2>&1 & local hs=$!
    sleep 1
    say "  CFNetwork http://$gw:8099/: $("$TD/session.sh" run $FJN http "http://$gw:8099/" 2>&1 | tr '\n' ' ')"
    kill $hs 2>/dev/null
    say "  CFNetwork http://example.com/: $("$TD/session.sh" run $FJN http "http://example.com/" 2>&1 | tr '\n' ' ')"
  fi
  say "  show en0 after: $("$TD/session.sh" run $FJN show en0 2>&1 | tr '\n' ' ')"
  say "  kcmd net: $("$TD/session.sh" call kcmd net 2>&1 | head -1)"
  kill -TERM "$b" 2>/dev/null; wait "$b" 2>/dev/null
  grep -E 'dhcp|icmp|arp' "$RES/ios-bridge.txt" | head -12 | while read -r r; do say "  bridge: $r"; done
  session_end
  if [ "$ok" = 1 ]; then verdict ios PASS "iOS en0 over the NET channel: DHCP lease $ip, ping $gw"
  else verdict ios FAIL "see above and $RES"; fi
}

for s in "${want[@]}"; do say "== $s"; "stage_$s"; done
say "== summary"
for s in "${want[@]}"; do printf '  %-7s %s\n' "$s" "${VERDICT[$s]:-?}" | tee -a "$REPORT"; done
[ "$fails" -eq 0 ] || { say "FAIL: $fails stage(s)"; exit 1; }
say "PASS: all requested stages"
