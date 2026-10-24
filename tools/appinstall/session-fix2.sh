#!/bin/bash
# Create installd's home tree (missing on this Data volume), then sideload Hello and launch it. Also watch the staged-app dir.
O=$1; S=$2; SS=$S/session.sh; R=$S/mig/s4; mkdir -p $R
$SS start $S/payload16-session.img > $R/start.out 2>&1; NAME=$(head -1 $R/start.out | cut -d: -f1)
trap '$SS end $NAME' EXIT
grep -q 'ready after' $R/start.out || exit 1
$SS run /usr/sbin/fsx /private/var/staged_system_apps 1 | head -3
for t in instapp fsx; do $SS push $O/$t /private/var/ngpu/sbin/$t | grep -E 'put ok'; done
$SS run /usr/sbin/fsx /private/var/installd 4 | head -12
D=/private/var/mobile/Media/PublicStaging
T=90 $SS run /usr/sbin/instapp ls $D/Hello.app org.unixnu.hello | tee $R/ls.txt | cut -c1-600
$SS run /usr/sbin/instapp list | grep -i hello
$SS run /usr/sbin/instapp launch org.unixnu.hello; sleep 5; $SS call screenshot $R/hello.png
$SS run ps -A | grep -i -E 'hello|installd' | cut -c1-100
$SS run /usr/sbin/fsx /private/var/installd 4 | head -30
$SS run /usr/sbin/fsx /private/var/containers/Bundle 3 | head -10
