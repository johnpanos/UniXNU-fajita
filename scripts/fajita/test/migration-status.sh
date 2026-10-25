#!/bin/bash
# migration-status.sh [OUTDIR]: one-shot view of iOS's first-boot data migration and Setup state on the running session (read-only).
# migration-status.sh --watch [SECONDS] [OUTDIR]: a timeline instead, every SECONDS (default 45) until interrupted: migration processes
#   with CPU time, staged apps left, app containers, and a screenshot (OUTDIR/tl-NNN.png); appended to OUTDIR/timeline.txt.
# The one-shot view:
#   - migration processes (DataMigrator, migrationpluginwrapper, installd, Setup/Buddy, SpringBoard) from ps
#   - com.apple.migration.plist (LastSystemVersion, DMLastMigrationResults: written by DataMigrator) and com.apple.purplebuddy.plist
#   - app bundle containers (/private/var/containers/Bundle/Application) and the staged apps still waiting
#   - installd's LaunchServices map (/var/installd/Library/MobileInstallation/LastLaunchServicesMap.plist: the apps registered)
#   - new crash and stackshot reports (DataMigrator writes a "stacks+com.apple.datamigrator" report naming a plugin it thinks is hung)
# Needs the fsx read helper on the phone (tools/appinstall/fsx.c, built into .cache/appinstall-tools/fsx; it is pushed on first use).
# A stalled migration: `session.sh run /usr/sbin/spindump PID 3 10 -stdout` (iOS ships spindump) shows what datamigrator waits on.
# Pulled files land in OUTDIR (default .cache/fajita-work/migration). Nothing on the phone is written except the fsx helper.
BOARD=$(cd "$(dirname "$0")/../../.." && pwd); SS=$BOARD/scripts/fajita/test/session.sh
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}
WATCH=0; [ "$1" = --watch ] && { WATCH=1; EVERY=${2:-45}; shift; [ -n "$1" ] && shift; }
O=${1:-$W/migration}; mkdir -p "$O"
FSX=$BOARD/.cache/appinstall-tools/fsx
for i in 1 2 3; do $SS run /usr/sbin/fsx 2>&1 | grep -q 'cannot run' || break; [ -f "$FSX" ] && $SS push "$FSX" /private/var/ngpu/sbin/fsx >/dev/null 2>&1; done
PSRE='migrat|installd|installcoord|budd|setup|springboard|ReportCrash'
staged() { $SS run /usr/sbin/fsx /private/var/staged_system_apps 0 2>&1 | grep -c -E "\.app/"; }
apps() { $SS run /usr/sbin/fsx /private/var/containers/Bundle/Application 1 2>&1 | grep -E "\.app/ "; }
if [ $WATCH = 1 ]; then
  n=$(ls "$O"/tl-*.png 2>/dev/null | wc -l)
  while true; do
    n=$((n + 1))
    { echo "### $(date +%T)"; $SS run /bin/ps -A -o pid,etime,time,comm 2>&1 | grep -i -E "$PSRE" | grep -v grep
      echo "staged: $(staged)  containers: $(apps | wc -l)"; } >> "$O/timeline.txt"
    $SS call screenshot "$O/tl-$(printf %03d $n).png" >/dev/null 2>&1
    sleep "$EVERY"
  done
fi
echo "== processes"
$SS run /bin/ps -A -o pid,etime,time,comm 2>&1 | grep -i -E "$PSRE" | grep -v grep
P=/private/var/mobile/Library/Preferences
show() {  # REMOTE NAME
  rm -f "$O/$2"; $SS pull "$1" "$O/$2" >/dev/null 2>&1
  if [ -s "$O/$2" ]; then python3 -I -c 'import plistlib,sys,pprint; pprint.pprint(plistlib.load(open(sys.argv[1],"rb")),width=150)' "$O/$2" 2>&1 | head -${3:-40}
  else echo "(absent)"; fi; }
echo "== com.apple.migration.plist"; show $P/com.apple.migration.plist migration.plist
echo "== com.apple.purplebuddy.plist"; show $P/com.apple.purplebuddy.plist purplebuddy.plist
echo "== bundle containers: $(apps | wc -l)"; apps | sed -E 's#/ [0-9]+$##; s#.*/##' | sort | tr '\n' ' '; echo
echo "== staged_system_apps left: $(staged)"
echo "== LaunchServices map (installd)"
rm -f "$O/lsmap.plist"; $SS pull /private/var/installd/Library/MobileInstallation/LastLaunchServicesMap.plist "$O/lsmap.plist" >/dev/null 2>&1
[ -s "$O/lsmap.plist" ] && python3 -I -c '
import plistlib,sys; d=plistlib.load(open(sys.argv[1],"rb"))
for k,v in d.items():
  if isinstance(v,dict): print(k, len(v), " ".join(sorted(v))[:1200])' "$O/lsmap.plist" || echo "(absent)"
echo "== crash and stackshot reports"
$SS run /usr/sbin/fsx /private/var/mobile/Library/Logs/CrashReporter 0 2>&1 | grep -E '\.ips ' | sed 's#.*/##' | grep -v -E '^(OTAUpdate|SiriSearchFeedback)'
