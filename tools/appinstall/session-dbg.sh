#!/bin/bash
O=$1; S=$2; SS=$S/session.sh; R=$S/mig/s6; mkdir -p $R
$SS start $S/payload16-session.img > $R/start.out 2>&1; NAME=$(head -1 $R/start.out | cut -d: -f1)
trap '$SS end $NAME' EXIT
grep -q 'ready after' $R/start.out || exit 1
for t in instapp fsx; do $SS push $O/$t /private/var/ngpu/sbin/$t | grep -E 'put ok'; done
C=/private/var/installd/Library/Caches; Z=$C/com.apple.mobile.installd.staging
$SS run /usr/sbin/fsx mk $Z 33 33
$SS run /usr/sbin/fsx tmpd $Z 0; $SS run /usr/sbin/fsx tmpd $Z 33
$SS run /usr/sbin/fsx /private/var/installd 3 | grep -E 'Caches'
T=90 $SS run /usr/sbin/instapp ls /private/var/mobile/Media/PublicStaging/Hello.app org.unixnu.hello | cut -c1-120
$SS run /usr/sbin/fsx /private/var/installd 3 | grep -E 'Caches'
