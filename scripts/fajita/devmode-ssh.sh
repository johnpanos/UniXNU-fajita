#!/bin/bash
# scripts/fajita/devmode-ssh.sh [find | ssh [ARGS...] | CMD...]: reach the phone's SSH dev mode (NOTES-devmode.md) over Wi-Fi.
#   devmode-ssh.sh find        print the phone's Wi-Fi address and the ssh command, exit 1 if it is not reachable
#   devmode-ssh.sh             an interactive login shell (same as `devmode-ssh.sh ssh`)
#   devmode-ssh.sh ssh ARGS    ssh to the phone with ARGS (e.g. -L 8080:127.0.0.1:80, or a command)
#   devmode-ssh.sh CMD...      run CMD on the phone
# The address is found, in order: $FAJITA_SSH_HOST; the last address that answered (.cache/fajita-work/devmode-addr); the
# phone's Wi-Fi MAC (tools/wifi/persist-wlan-mac.py, or $FAJITA_WIFI_MAC) in the host's neighbour table, after a ping sweep of the
# host's own /24 when needed; and, when the USB debug channel is up, en0's address as the phone reports it. ssh uses
# HostKeyAlias=fajita, so the host key is checked whatever address DHCP gives the phone.
set -o pipefail
BOARD=$(cd "$(dirname "$0")/../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}; mkdir -p "$W"
PORT=${FAJITA_SSH_PORT:-22}
SSH_OPTS=(-o HostKeyAlias=fajita -o ConnectTimeout=5 -o ServerAliveInterval=15 -p "$PORT")
open22() { timeout 3 bash -c "exec 3<>/dev/tcp/$1/$PORT && head -c 4 <&3" 2>/dev/null | grep -q '^SSH-'; }
mac=${FAJITA_WIFI_MAC:-$(python3 -I "$BOARD/tools/wifi/persist-wlan-mac.py" 2>/dev/null)}
mac=$(echo "$mac" | tr 'A-F' 'a-f' | sed -E 's/[^0-9a-f]//g; s/(..)(..)(..)(..)(..)(..)/\1:\2:\3:\4:\5:\6/')
by_mac() { [ -n "$mac" ] && ip neigh show | awk -v m="$mac" 'tolower($0) ~ m && $1 ~ /^[0-9.]+$/ {print $1; exit}'; }
find_phone() {
  local a
  for a in "$FAJITA_SSH_HOST" "$(cat "$W/devmode-addr" 2>/dev/null)"; do [ -n "$a" ] && open22 "$a" && { echo "$a"; return 0; }; done
  a=$(by_mac); [ -n "$a" ] && open22 "$a" && { echo "$a"; return 0; }
  # a ping sweep of the host's own /24 fills the neighbour table
  local net; net=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "src") print $(i + 1)}' | cut -d. -f1-3)
  if [ -n "$net" ] && [ -n "$mac" ]; then
    for i in $(seq 1 254); do ping -c1 -W1 "$net.$i" >/dev/null 2>&1 & done; wait
    a=$(by_mac); [ -n "$a" ] && open22 "$a" && { echo "$a"; return 0; }
  fi
  # the USB debug channel, when a hub is serving
  if pgrep -f "fajita-usbdbg-hub.py serve" >/dev/null; then
    a=$(python3 -I "$BOARD/scripts/fajita-usbdbg-hub.py" call --timeout 10 run /usr/sbin/ifconfig en0 2>/dev/null | awk '/inet /{print $2; exit}')
    [ -n "$a" ] && open22 "$a" && { echo "$a"; return 0; }
  fi
  return 1
}
addr=$(find_phone) || { echo "the phone's SSH dev mode is not reachable (mac ${mac:-unknown}; is it booted with fajita-ssh=1 and on Wi-Fi?)" >&2; exit 1; }
echo "$addr" > "$W/devmode-addr"
case $1 in
  find) echo "$addr"; echo "ssh ${SSH_OPTS[*]} root@$addr"; echo "scp -o HostKeyAlias=fajita -P $PORT FILE root@$addr:/private/var/root/"; exit 0 ;;
  ssh) shift; exec ssh "${SSH_OPTS[@]}" "root@$addr" "$@" ;;
  '') exec ssh "${SSH_OPTS[@]}" "root@$addr" ;;
  *) exec ssh "${SSH_OPTS[@]}" "root@$addr" "$@" ;;
esac
