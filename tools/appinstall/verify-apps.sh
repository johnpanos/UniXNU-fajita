#!/bin/bash
# verify-apps.sh SCRATCH OUTDIR TAG: on a RUNNING session, report for each app: registered (instapp list), bundle present on disk, and launches
# (tap its icon, ps for the process, one screenshot per app in SCRATCH/mig/verify-TAG/). Home layout (half-res screenshot x2 = panel coords):
# page 1 Calendar Photos Mail Notes / Reminders Clock TV AppStore / Settings Hello; page 2 starts with Weather Contacts.
S=$1; O=$2; TAG=$3; SS=$S/session.sh; R=$S/mig/verify-$TAG; mkdir -p $R
for t in instapp fsx; do $SS push $O/$t /private/var/ngpu/sbin/$t | grep -q 'put ok' || echo "push $t failed"; done
$SS run /usr/sbin/instapp list > $R/list.txt 2>&1
$SS run /usr/sbin/fsx /private/var/containers/Bundle/Application 1 > $R/bundles.txt 2>&1
$SS call kcmd touch swipe up > /dev/null; sleep 3
declare -A POS=( [mobilecal]="164 840:MobileCal" [mobilemail]="664 840:MobileMail" [mobilenotes]="914 840:MobileNotes" [reminders]="164 1114:Reminders" [mobiletimer]="414 1114:MobileTimer" [tv]="664 1114:AppleTV" [hello]="414 1388:Hello" [weather]="164 300:Weather" [MobileAddressBook]="414 300:Contacts" )
printf '%-18s %-10s %-8s %s\n' app registered bundle launches
for k in mobilecal mobilemail mobilenotes reminders mobiletimer tv hello weather MobileAddressBook; do
  xy=${POS[$k]%%:*}; name=${POS[$k]##*:}
  reg=$(grep -c -i "\.$k\$\|\.$k\.\|^org.unixnu.$k\$" $R/list.txt); b=$(grep -c "/$name.app/ " $R/bundles.txt)
  page=0; case $k in weather|MobileAddressBook) page=1;; esac
  [ $page = 1 ] && [ -z "$onp2" ] && { $SS call kcmd touch swipe left > /dev/null; sleep 2; onp2=1; }
  $SS call kcmd touch tap $xy > /dev/null; sleep 7
  l=$($SS run ps -A | grep -c "/$name.app/$name\|/$name\$")
  $SS call screenshot $R/$name.png > /dev/null
  printf '%-18s %-10s %-8s %s\n' $k $reg $b "$l"
  $SS call kcmd touch swipe up > /dev/null; sleep 3
done
