#!/bin/bash
# Short diagnostic session: staged-app marker/dir state, sideload Hello with an explicit bundle id, launch, screenshot.
O=$1; S=$2; SS=$S/session.sh; R=$S/mig/s3; mkdir -p $R
$SS start $S/payload16-session.img > $R/start.out 2>&1; NAME=$(head -1 $R/start.out | cut -d: -f1)
trap '$SS end $NAME' EXIT
grep -q 'ready after' $R/start.out || exit 1
for t in instapp fsx; do $SS push $O/$t /private/var/ngpu/sbin/$t | grep -E 'put ok'; done
$SS run /usr/sbin/fsx /private/var/db 0 | grep -i -E 'sheng|staged'
$SS run /usr/sbin/fsx /private/var/staged_system_apps 1 | head -5
$SS call state 2>&1 | head -5
D=/private/var/mobile/Media/PublicStaging
$SS run /usr/sbin/fsx /private/var/mobile/Media/PublicStaging 1 | head
T=90 $SS run /usr/sbin/instapp ls $D/Hello.app org.unixnu.hello | tee $R/ls.txt | cut -c1-400
$SS run /usr/sbin/instapp list | grep -i hello
$SS run /usr/sbin/instapp launch org.unixnu.hello; sleep 5; $SS call screenshot $R/hello.png
