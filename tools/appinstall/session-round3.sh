#!/bin/bash
# On a RUNNING session: reinstall Weather (16.7 set), tap Contacts page-aware, retest Notes with 20 s, and diagnose Mail's maild.
S=$1; O=$2; ST=$3; SS=$S/session.sh; H=$(cd "$(dirname "$0")" && pwd); R=$S/mig/r3; mkdir -p $R
L=$(ls -t $S/logs/*/usb-console.txt | head -1)
tapshot() { # name x y wait [page2]
  $SS call kcmd touch swipe up >/dev/null; sleep 3
  [ -n "$5" ] && { $SS call kcmd touch swipe left >/dev/null; sleep 2; }
  n=$(wc -l < $L); $SS call kcmd touch tap $2 $3 >/dev/null; sleep $4
  echo "== $1 ps: $($SS run ps -A | grep -c "/$1.app/$1")"; $SS call screenshot $R/$1.png >/dev/null
  tail -n +$n $L | grep -a -E 'CRASH|maild' | grep -a -v aggregated | cut -c1-170 | head -5
}
$H/install-apps.sh $S $O $ST Weather | tail -3
$SS call kcmd touch swipe up >/dev/null; sleep 2
tapshot Weather 164 280 12 p2
tapshot Contacts 414 280 12 p2
tapshot MobileNotes 914 840 20
tapshot MobileMail 664 840 12
$SS run ps -A | grep -i -E 'maild|mailserver' | cut -c1-120
$SS run /System/Library/PrivateFrameworks/EmailDaemon.framework/maild 2>&1 | head -6 | cut -c1-250
$SS run /usr/sbin/fsx /private/var/mobile/Library/Mail 0 2>&1 | head -3
