#!/bin/bash
# orphan-reset.sh: put a phone that is up with nobody holding it (its cycle.sh was killed with the job that started the session: the
# fajita usb-debug gadget is on USB, no cycle.sh and no hub run) back into fastboot, through a hub of its own for the one reset call.
# Refuses while any cycle.sh runs (that phone is someone's session: use session.sh end NAME).
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}
SOCK=${FAJITA_USBDBG_HUB:-${XDG_RUNTIME_DIR:-/tmp}/fjdbg-hub.sock}
if ps -eo args | grep -q '^/bin/bash .*cycle.sh '; then echo "orphan-reset: a cycle.sh runs: not an orphan"; exit 1; fi
lsusb | grep -q '1209:4d69' || { echo "orphan-reset: no fajita usb-debug gadget on USB (already in fastboot?)"; fastboot devices; exit 0; }
rm -f "$SOCK"
python3 -I "$BOARD/scripts/fajita-usbdbg-hub.py" serve --log "$W/logs/orphan-reset-$(date +%H%M%S).txt" --sock "$SOCK" & hub=$!
trap 'kill $hub 2>/dev/null; rm -f "$SOCK"' EXIT
timeout 20 bash -c "until [ -S '$SOCK' ]; do sleep 1; done" || { echo "orphan-reset: the hub did not start"; exit 1; }
timeout 30 python3 -I "$BOARD/scripts/fajita-usbdbg-hub.py" call --sock "$SOCK" reset
timeout 60 bash -c 'until fastboot devices | grep -q fastboot; do sleep 2; done' && echo "orphan-reset: in fastboot" || { echo "orphan-reset: no fastboot after 60 s"; exit 1; }
