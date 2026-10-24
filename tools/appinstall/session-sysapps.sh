#!/bin/bash
# Session 2: staged apps recopied at boot (marker removed) -> sideload Hello via LaunchServices, then sysappmig (sampling) and per-app fallback.
O=$1; S=$2; SS=$S/session.sh; R=$S/mig/s2; mkdir -p $R
$SS start $S/payload16-session.img > $R/start.out 2>&1; NAME=$(head -1 $R/start.out | cut -d: -f1)
trap '$SS end $NAME' EXIT
grep -q 'ready after' $R/start.out || exit 1
for t in sysappmig instapp fsx; do $SS push $O/$t /private/var/ngpu/sbin/$t | grep -E 'put ok'; done
$SS run /usr/sbin/fsx /private/var/staged_system_apps 0 | tee $R/staged.txt | head -40
D=/private/var/mobile/Media/PublicStaging
$SS run /usr/sbin/fsx mk $D/Hello.app
$SS push $O/Hello.app/Info.plist $D/Hello.app/Info.plist 644 | grep put
$SS push $O/Hello.app/Hello $D/Hello.app/Hello 755 | grep put
T=90 $SS run /usr/sbin/instapp ls $D/Hello.app | tee $R/ls.txt
$SS run /usr/sbin/instapp list | tee $R/list1.txt | grep -i -E 'hello|org\.'
$SS run /usr/sbin/instapp launch org.unixnu.hello
$SS run /usr/sbin/sysappmig bg; sleep 200
$SS pull /var/mobile/sysappmig.log $R/mig.log >/dev/null; grep -v '^  0x' $R/mig.log | tail -12
$SS run /usr/sbin/instapp list | tee $R/list2.txt | wc -l
$SS run /usr/sbin/fsx /private/var/staged_system_apps 0 | tee $R/staged2.txt | head -3
