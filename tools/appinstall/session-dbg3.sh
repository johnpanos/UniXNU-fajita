#!/bin/bash
O=$1; S=$2; SS=$S/session.sh; R=$S/mig/s8; mkdir -p $R
$SS start $S/payload16-session.img > $R/start.out 2>&1; NAME=$(head -1 $R/start.out | cut -d: -f1)
trap '$SS end $NAME' EXIT
grep -q 'ready after' $R/start.out || exit 1
$SS push $O/instapp /private/var/ngpu/sbin/instapp | grep -E 'put ok'
Z=/private/var/installd/Library/Caches/com.apple.mobile.installd.staging
$SS run /usr/sbin/fsx mk $Z 33 33
$SS run /usr/sbin/instapp -root mkt $Z | cut -c1-500
