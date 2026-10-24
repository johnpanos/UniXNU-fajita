#!/bin/bash
O=$1; S=$2; SS=$S/session.sh; R=$S/mig/s10; mkdir -p $R
$SS start $S/payload16-session.img > $R/start.out 2>&1; NAME=$(head -1 $R/start.out | cut -d: -f1)
trap '$SS end $NAME' EXIT
grep -q 'ready after' $R/start.out || exit 1
$SS push $O/instapp /private/var/ngpu/sbin/instapp | grep -E 'put ok'
$SS run ps -A | grep -i -E 'SpringBoard|backboardd|frontboard|runningboard|Preferences|launchd|assertiond' | cut -c1-110
$SS run /usr/sbin/instapp list | grep -c .
$SS run /usr/sbin/instapp list | grep -i -E 'hello|Preferences'
$SS run /usr/sbin/instapp launch com.apple.Preferences; sleep 6; $SS call screenshot $R/prefs.png
$SS run ps -A | grep -i -E 'Preferences' | cut -c1-110
