#!/bin/bash
# cryptex-jobs.sh: launchd jobs of the running (held) session whose program lives under /System/Cryptexes (the OS volume's
# symlinks to /private/preboot/Cryptexes/{OS,App}, which a board without the cryptex graft does not provide).
# Usage: scripts/fajita/test/cryptex-jobs.sh [LAUNCHDDIR]   -> "label program" lines; pulls into .cache/fajita-work/ld
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd); S=$TD/session.sh
D=${1:-/System/Library/LaunchDaemons}; W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}/ld; mkdir -p "$W"
"$S" run /usr/sbin/fjbundle --ls "$D" 2>&1 | tr ' ' '\n' | grep -a '\.plist$' | while read -r f; do
  "$S" pull "$D/$f" "$W/$f" >/dev/null 2>&1
  python3 -I - "$W/$f" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
p = d.get('Program') or ''
a = d.get('ProgramArguments') or []
for x in [p] + a[:1]:
    if '/System/Cryptexes/' in x:
        print(d.get('Label'), x)
PY
done
