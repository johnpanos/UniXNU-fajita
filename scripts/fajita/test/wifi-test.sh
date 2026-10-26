#!/bin/bash
# wifi-test.sh: the fajita Wi-Fi acceptance suite (WCN3990 on the SDM845 modem). Its stages match the bring-up of
# NOTES-wifi.md; each prints PASS/FAIL with its key log lines, and the script exits non-zero if any requested stage
# failed. Unattended and self-recovering: it builds a --test image (autoreset + watchdog backstop), boots it once in a
# held session, and evaluates the stages from the captured UFS/USB log and from iOS-side probes over the USB debug
# channel. Run it through bg.sh:
#
#   scripts/fajita/test/bg.sh start wifi NAME --max 1500 scripts/fajita/test/wifi-test.sh all
#   scripts/fajita/test/wifi-test.sh power modem link        # a subset
#   WIFI_NO_BUILD=1 scripts/fajita/test/wifi-test.sh scan    # reuse the last .cache/fajita-out-wifi image
#
# Stages (each maps to a fajita-wifi=<N> depth and a pass check):
#   power     fajita-wifi=1  regulators/clocks mapped and read (FW1, RMB readable, modem not yet booted)
#   modem     fajita-wifi=2  MSS remoteproc up (MBA auth, mpss segments, AUTH_COMPLETE, SMP2P ready)
#   link      fajita-wifi=3  SMEM + GLINK + QRTR; the modem announces its QMI servers
#   servers   fajita-wifi=4  AP-side pd-mapper / tqftpserv / rmtfs answer the modem
#   wlfw      fajita-wifi=5  the wlfw handshake completes, FW_READY
#   scan      fajita-wifi=6  CE/HTC/WMI ready; a scan sees the test SSID with an RSSI
#   assoc     fajita-wifi=7  associate + the WPA2-PSK 4-way handshake (PTK/GTK installed)
#   dhcp      fajita-wifi=7  iOS gets a DHCP lease on the Wi-Fi interface
#   ping      fajita-wifi=7  ping the gateway
#   tput      fajita-wifi=7  a TCP throughput test against this laptop on the LAN
#   stability fajita-wifi=7  a reconnect loop stays associated and keeps passing traffic
#
# Credentials: ~/.config/unixnu/wifi-kittycorner (SSID=, PSK=), mode 600, outside git. The SSID travels as a boot arg;
# the PSK is pushed to the phone at runtime (/private/var/ngpu/wifi.psk) and is NEVER printed, logged or committed. Logs
# only ever say the SSID and "psk set".
set -uo pipefail
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}
OUT=${FAJITA_OUT:-$ROOT/.cache/fajita-out-wifi}
CREDS=${WIFI_CREDS:-$HOME/.config/unixnu/wifi-kittycorner}
HUB="python3 -I $TD/../fajita-usbdbg-hub.py"
HUBT=${WIFI_HUB_TIMEOUT:-20}            # short hub timeouts, per AGENTS
# rmtfs (fajita DT 0xf5b01000+0x200000 with guard pages), wlan msa, mpss, mba (sdm845.dtsi reserved-memory)
PHYS_RESERVE=${WIFI_PHYS_RESERVE:-0xf5b00000.0x204000,0x8df00000.0x100000,0x8e000000.0x7800000,0x96500000.0x200000}
ALL=(power modem link servers wlfw scan assoc dhcp ping tput stability)
declare -A DEPTH=( [power]=1 [modem]=2 [link]=3 [servers]=4 [wlfw]=5 [scan]=6 [assoc]=7 [dhcp]=7 [ping]=7 [tput]=7 [stability]=7 )

want=("$@"); [ ${#want[@]} -eq 0 ] && want=(power)
[ "${want[0]:-}" = all ] && want=("${ALL[@]}")
for s in "${want[@]}"; do [ -n "${DEPTH[$s]:-}" ] || { echo "unknown stage '$s'; known: ${ALL[*]}"; exit 2; }; done

# Highest depth requested -> the fajita-wifi=N the image is built with (each stage runs all lower ones).
maxN=1; for s in "${want[@]}"; do [ "${DEPTH[$s]}" -gt "$maxN" ] && maxN=${DEPTH[$s]}; done

ssid=; needpsk=0
if [ -r "$CREDS" ]; then ssid=$(awk -F= '/^SSID=/{print $2}' "$CREDS"); fi
for s in "${want[@]}"; do [ "${DEPTH[$s]}" -ge 6 ] && needpsk=1; done
if [ "$needpsk" = 1 ]; then
  [ -r "$CREDS" ] || { echo "stages scan+ need $CREDS (SSID=, PSK=), mode 600"; exit 2; }
  [ -n "$ssid" ] || { echo "no SSID in $CREDS"; exit 2; }
fi

RES=$W/logs/wifi-test; mkdir -p "$RES"; REPORT=$RES/report.txt; : > "$REPORT"
say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$REPORT"; }
fails=0; declare -A VERDICT

# --- build one --test image at fajita-wifi=$maxN (never committed; PSK is not a boot arg) ---
build() {
  [ -n "${WIFI_NO_BUILD:-}" ] && { [ -f "$OUT/payload.img" ] && return 0; echo "WIFI_NO_BUILD set but no image at $OUT"; exit 1; }
  local args=(fajita-wifi=$maxN fajita-wifi-dump=1 native-ufs-partread=1   # partread: rmtfs reads the EFS partitions (read-only)
              "phys-reserve=$PHYS_RESERVE")                                 # the modem carve-outs, off XNU's free lists
  [ -n "$ssid" ] && args+=("fajita-wifi-ssid=$ssid")   # SSID is not secret
  say "build: --test ${args[*]} (autoreset backstop)"
  git -C "$ROOT" pull --no-rebase origin ios16-port >/dev/null 2>&1 || true
  FAJITA_OUT=$OUT FAJITA_AUTORESET=${WIFI_AUTORESET:-900} "$TD/../build.sh" --test --skip-plugin "${args[@]}" \
    > "$RES/build.txt" 2>&1 || { say "BUILD FAILED (see $RES/build.txt)"; tail -6 "$RES/build.txt" | tee -a "$REPORT"; exit 1; }
  say "build: $(grep -a '^\[' "$RES/build.txt" | tail -1 | sed 's#.*-> ##')"
}

# --- a held session on that image ---
NAME=
session_start() {
  say "session: boot fajita-wifi=$maxN"
  FAJITA_OUT=$OUT "$TD/session.sh" start >/dev/null 2>&1 || { say "session start failed"; exit 1; }
  NAME=$(cat "$W/session.current")
  CON=$W/logs/$NAME/usb-console.txt
  say "session $NAME (console $CON)"
}
session_end() { [ -n "$NAME" ] && "$TD/session.sh" end "$NAME" >/dev/null 2>&1; }
trap session_end EXIT

# push the PSK once the agent is up, redacted. The driver/kcmd reads /private/var/ngpu/wifi.psk.
push_psk() {
  [ "$needpsk" = 1 ] || return 0
  local tmp; tmp=$(mktemp)
  awk -F= '/^PSK=/{print $2}' "$CREDS" > "$tmp"
  if $HUB call --timeout "$HUBT" push "$tmp" /private/var/ngpu/wifi.psk 600 >/dev/null 2>&1; then say "psk set (ssid $ssid)"
  else say "psk push failed"; fi
  shred -u "$tmp" 2>/dev/null || rm -f "$tmp"
}

# grep the captured console for a stage's pass pattern; record the matching line(s).
con_has() { grep -aE "$1" "$CON" 2>/dev/null | tail -2; }
verdict() { local s=$1 ok=$2; shift 2; VERDICT[$s]=$ok; if [ "$ok" = PASS ]; then say "PASS $s: $*"; else say "FAIL $s: $*"; fails=$((fails+1)); fi; }

# iOS-side probe over the channel (short timeout); echoes output.
ios() { $HUB call --timeout "$HUBT" run "$@" 2>/dev/null; }

check_from_log() {   # $1 stage  $2 pass-regex  $3 description
  local s=$1 re=$2 desc=$3 line
  line=$(con_has "$re")
  if [ -n "$line" ]; then verdict "$s" PASS "$(echo "$line" | tail -1)"
  else verdict "$s" FAIL "$desc (no match for /$re/ in console)"; fi
}

# Which layer of the link stopped, from the last FW3 row (FajitaWifiStack.cpp), lowest first.
layers() {
  local r; r=$(con_has 'FW3 ' | tail -1)
  [ -n "$r" ] || { say "  layer: no FW3 row (stack not reached: fajita-wifi<3, or the worker stopped at stage 2)"; return; }
  say "  $r"
  case $r in
    *'smem FAILED'*)        say "  layer: SMEM (map or header: SBL did not initialise it?)" ;;
    *'glink smem items FAILED'*) say "  layer: SMEM allocation of the GLINK descriptor/fifo (hwlock or partition full)" ;;
    *'fifo1 no'*)           say "  layer: modem not up (it never allocated its GLINK fifo 480): stage 2 / SMP2P ready" ;;
    *'glink v?'*)           say "  layer: GLINK version handshake (APCS IPC bit 12 kick or the modem's interrupt)" ;;
    *'GLINK-FAULT'*)        say "  layer: GLINK protocol fault (unknown command or corrupt indices)" ;;
    *'ipcrtr closed'*)      say "  layer: GLINK IPCRTR channel open" ;;
    *'hello 0'*)            say "  layer: QRTR HELLO from the modem" ;;
    *'servers -'*)          say "  layer: the modem announced no QMI servers yet" ;;
    *)                      say "  layer: link up; servers announced: ${r##*servers }" ;;
  esac
}

evaluate() {
  for s in "${want[@]}"; do case $s in
    power) check_from_log power 'FW1 .*rmb pbl' 'stage 1 power/regs did not report'
           local r; r=$(con_has 'FW1 res'); [ -n "$r" ] && say "  $(echo "$r" | tail -1)" ;;
    modem) check_from_log modem 'FW2 .*auth[1-9].*ready' 'MSS did not reach AUTH_COMPLETE + ready' ;;
    link)  check_from_log link  'FW3 smp2p .* glink v1 .* ipcrtr open qrtr hello 1 .* servers ([0-9a-f]+,)*45(,|$| )' \
             'no GLINK/QRTR link with wlfw (0x45) announced'
           layers ;;
    servers) check_from_log servers 'FW4 pdr [1-9][0-9]* tftp rrq[1-9]' 'the modem did not use pd-mapper and TFTP'
           local r; r=$(con_has 'FW4 '); [ -n "$r" ] && say "  $(echo "$r" | tail -1)"
           r=$(con_has 'FW4 services'); [ -n "$r" ] && say "  $(echo "$r" | tail -1)" ;;
    wlfw)  check_from_log wlfw  'FW5 wlfw enabled ' 'wlfw handshake did not reach FW_READY + wlan_cfg/wlan_mode'
           local r; r=$(con_has 'FW5 '); [ -n "$r" ] && say "  $(echo "$r" | tail -1)"
           case $r in *msa-assign?*) say "  layer: no fajita_wifi_msa_assign hook (the modem-boot side assigns the MSA)" ;;
                      *'failed in '*) say "  layer: wlfw step ${r#*failed in } (result/error in the row)" ;; esac ;;
    scan)  check_from_log scan  "FW6 .*scan.*$ssid" "scan did not see $ssid" ;;
    assoc) check_from_log assoc 'FW7 .*(ptk|gtk|connect)' '4-way handshake / association did not complete' ;;
    dhcp)
      local o; o=$(ios ipconfig getifaddr en1 2>/dev/null || true)
      if echo "$o" | grep -qE '([0-9]+\.){3}[0-9]+'; then verdict dhcp PASS "lease $o"
      else verdict dhcp FAIL "no DHCP lease on the Wi-Fi interface (en1): ${o:-none}"; fi ;;
    ping)
      local gw o; gw=$(ios route -n get default 2>/dev/null | awk '/gateway/{print $2}')
      [ -n "$gw" ] && o=$(ios ping -c 3 -t 3 "$gw" 2>/dev/null)
      if echo "$o" | grep -q '3 packets received\|0.0% packet loss'; then verdict ping PASS "gateway $gw reachable"
      else verdict ping FAIL "gateway ${gw:-unknown} not reachable"; fi ;;
    tput)
      # TCP throughput against this laptop: iOS has no iperf, so time a bulk GET from a tiny laptop-side server.
      # Until the link exists this reports FAIL; see NOTES-wifi.md for the laptop-side helper plan.
      local me; me=$(ip route get 1.1.1.1 2>/dev/null | awk '/src/{print $NF; exit}')
      local o; o=$(ios sh -c "echo probe" 2>/dev/null)
      verdict tput FAIL "throughput against $me not yet runnable (Wi-Fi data path pending; agent reachable: ${o:+yes})" ;;
    stability)
      verdict stability FAIL "reconnect loop pending the data path" ;;
  esac; done
}

say "wifi-test: stages ${want[*]} (build fajita-wifi=$maxN)${ssid:+, ssid $ssid}"
build
session_start
push_psk
# give the staged probe time to run from the boot progress monitor (it waits for the watchdog arm, then runs the stages)
say "waiting up to ${WIFI_SETTLE:-180}s for the bring-up stages to log"
# Wait for the deepest requested stage's pass row (the link and the services come up seconds after FW3/FW4 first log),
# or a row that says it will not get there.
declare -A DONE_RE=( [1]='FW1 .*rmb' [2]='FW2 ' [3]='FW3 smp2p .* servers [0-9a-f]' [4]='FW4 pdr [1-9][0-9]* tftp rrq[1-9]'
                     [5]='FW5 wlfw (enabled|failed)' [6]='FW6 ' [7]='FW7 ' )
for i in $(seq 1 $(( ${WIFI_SETTLE:-180} / 5 )) ); do
  grep -aqE "${DONE_RE[$maxN]}|not yet implemented|FAILED|reserve\?" "$CON" 2>/dev/null && break; sleep 5
done
evaluate
session_end; trap - EXIT

say "== summary"
for s in "${want[@]}"; do printf '  %-9s %s\n' "$s" "${VERDICT[$s]:-?}" | tee -a "$REPORT"; done
say "report: $REPORT; console: ${CON:-}"
[ "$fails" -eq 0 ] || { say "FAIL: $fails stage(s)"; exit 1; }
say "PASS: all requested stages"
