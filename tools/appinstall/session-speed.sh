#!/bin/bash
# push-speed probe on the shipping GPU payload; args: OUTDIR SCRATCH PAYLOAD BOOT
O=$1; S=$2; SS=$S/session.sh; R=$S/mig/s11; mkdir -p $R
$SS start $3 $4 > $R/start.out 2>&1; NAME=$(head -1 $R/start.out | cut -d: -f1)
trap '$SS end $NAME' EXIT
grep -q 'ready after' $R/start.out || exit 1
for t in instapp fsx; do $SS push $O/$t /private/var/ngpu/sbin/$t | grep -E 'put ok'; done
date +%s.%N > $R/t0; $SS push $S/mig/test4m.bin /private/var/mobile/test4m.bin 644 | grep put; date +%s.%N > $R/t1
$SS call screenshot $R/home.png
