#!/bin/bash
# biome-survey.sh [DIR...]: on the held session, what the Biome/CoreDuet stores hold and what coreduetd has open. Prints the phone's clock,
# the 25 largest files and the newest/oldest mtimes under each DIR (default: mobile's and root's Biome trees and the CoreDuet
# knowledge store), then coreduetd's and dasd's open files (fjbundle --fds). Full listings in $FAJITA_WORK/biome-survey/.
#   biome-survey.sh                      the default trees
#   PULL=PATH biome-survey.sh            also copy PATH off the phone into $FAJITA_WORK/biome-survey/ (then: tools/biome/segb.py FILE)
# fjbundle comes from $FAJITA_OUT/tools (scripts/fajita/build-usbdbg-agent.sh); it is pushed under a name derived from its hash.
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}; OUTD=${FAJITA_OUT:-$BOARD/.cache/fajita-out}; SS=$TD/session.sh
f=$OUTD/tools/fjbundle; [ -f "$f" ] || { echo "no $f (scripts/fajita/build-usbdbg-agent.sh $OUTD/tools/fjdbg-agent)"; exit 1; }
p=/private/var/ngpu/sbin/fjbundle-$(sha1sum "$f" | cut -c1-8)
T=120 $SS push "$f" $p >/dev/null 2>&1 || { echo "push failed"; exit 1; }
D=$W/biome-survey; mkdir -p $D
dirs=("$@"); [ ${#dirs[@]} -gt 0 ] || dirs=(/private/var/mobile/Library/Biome /private/var/db/biome /private/var/mobile/Library/CoreDuet
  /private/var/mobile/Library/DuetExpertCenter /private/var/root/Library/Biome)
for d in "${dirs[@]}"; do
  n=$(echo "$d" | tr / _)
  T=60 $SS run $p --find "$d" 10 > $D/$n.txt 2>&1
  echo "== $d: $(tail -1 $D/$n.txt)"
  head -1 $D/$n.txt
  grep -v "/ mode" $D/$n.txt | grep " size " | sort -t' ' -k3 -n -r | head -25 | awk '{printf "  %10s  mtime %s  pclass %s  %s\n", $3, $6, $10, $1}'
  grep " size " $D/$n.txt | awk '{print $5}' | sort -n | awk 'NR==1{lo=$1} {hi=$1} END{if (NR) print "  mtimes " lo " .. " hi}'
  grep -c "cannot open\|lstat:" $D/$n.txt | sed 's/^/  unreadable: /'
done
for proc in coreduetd dasd; do echo "== open files: $proc"; T=20 $SS run $p --fds $proc 2>&1 | grep -v "type [^1]" | head -60; done
if [ -n "$PULL" ]; then T=120 $SS pull "$PULL" $D/$(basename "$PULL") && echo "pulled $D/$(basename "$PULL")"; fi
