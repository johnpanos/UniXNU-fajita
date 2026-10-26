#!/bin/bash
# cryptex-jobs.sh: launchd jobs of the running (held) session whose program lives under /System/Cryptexes (the OS volume's symlinks to
# /private/preboot/Cryptexes/{OS,App}: the App cryptex is mounted there with fajita-appos=1, package/BootCryptexMount.c).
# Usage: scripts/fajita/test/cryptex-jobs.sh [--ping] [LAUNCHDDIR]
#   default  "label program" lines; plists pulled into .cache/fajita-work/ld
#   --ping   also, per job: look up its first Mach service and send it a message (fjbundle --lookup --ping --watch), so launchd starts
#            an on-demand job; prints "ran pid N", "disabled" (BOOTSTRAP_UNKNOWN_SERVICE: off in launchd's overrides,
#            package/BootWatchdogPolicy.c) or "NOT SPAWNED" (found, but no process appeared within 3 s). Needs fjbundle built by
#            scripts/fajita/build-usbdbg-agent.sh ($FJBUNDLE, default .cache/fajita-inputs/fjbundle); it is pushed under a name of its hash.
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd); S=$TD/session.sh
PING=0; [ "$1" = --ping ] && { PING=1; shift; }
D=${1:-/System/Library/LaunchDaemons}; W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}/ld; mkdir -p "$W"
if [ $PING = 1 ]; then
  FB=${FJBUNDLE:-$ROOT/.cache/fajita-inputs/fjbundle}; [ -f "$FB" ] || { echo "no fjbundle at $FB (scripts/fajita/build-usbdbg-agent.sh)"; exit 1; }
  NAME=fjbundle-$(sha256sum "$FB" | cut -c1-8)   # a changed build under a new name: an overwritten path ran the old binary
  "$S" push "$FB" /private/var/ngpu/sbin/$NAME >/dev/null 2>&1
fi
"$S" run /usr/sbin/fjbundle --ls "$D" 2>&1 | tr ' ' '\n' | grep -a '\.plist$' | while read -r f; do
  [ -s "$W/$f" ] || "$S" pull "$D/$f" "$W/$f" >/dev/null 2>&1   # sealed-volume plists: a cached copy is current
  line=$(python3 -I - "$W/$f" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
p = d.get('Program') or ''
a = d.get('ProgramArguments') or []
for x in [p] + a[:1]:
    if '/System/Cryptexes/' in x:
        print(d.get('Label'), x, next(iter(d.get('MachServices') or {}), '-'))
        break
PY
)
  [ -n "$line" ] || continue
  read -r label program service <<<"$line"
  if [ $PING = 0 ]; then echo "$label $program"; continue; fi
  out=$("$S" run /usr/sbin/$NAME --lookup --ping --watch "$(basename "$program")" "$service" </dev/null 2>&1 | head -1)
  case $out in
    *'(ran)'*) state="ran pid $(echo "$out" | sed -n 's/.* pid \([0-9]*\) (ran).*/\1/p')" ;;
    *'look_up 1102'*) state=disabled ;;
    *'look_up 0'*) state="NOT SPAWNED" ;;
    *) state="? ${out:0:80}" ;;
  esac
  printf '%-66s %-14s %s\n' "$label" "$state" "$program"
done
