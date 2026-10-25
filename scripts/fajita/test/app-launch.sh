#!/bin/bash
# app-launch.sh [OUTDIR] [NAME...]: open every app of the stock iOS 16 home screen (both pages, the Utilities folder, the dock) by
# tapping its icon on the running session, then per app: a screenshot (OUTDIR/app-NAME.png), whether its process runs, and the
# kernel's CRASH rows meanwhile. Prints a table and writes it to OUTDIR/apps.tsv. Default OUTDIR .cache/fajita-work/apps.
# The phone must be on the home screen, page 1 (after the lock screen: `session.sh call kcmd key power`, then `touch swipe up`).
# NAMEs limit the run to some apps. WAIT (default 10) seconds per launch.
# Positions are the stock first-boot layout at 540x1170 screenshot scale (panel pixels are twice that).
BOARD=$(cd "$(dirname "$0")/../../.." && pwd); SS=$BOARD/scripts/fajita/test/session.sh
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}; O=${1:-$W/apps}; shift; mkdir -p "$O"
WAIT=${WAIT:-10}; ONLY=" $* "
LOG=$W/logs/$(cat "$W/session.current")/usb-console.txt
# name page x y process-path-fragment ("f" page: inside the Utilities folder on page 2; "d": the dock, from page 2)
APPS="FaceTime 1 82 418 FaceTime.app
Calendar 1 207 418 MobileCal.app
Photos 1 332 418 MobileSlideShow.app
Mail 1 458 418 MobileMail.app
Notes 1 82 555 MobileNotes.app
Reminders 1 207 555 Reminders.app
Clock 1 332 555 MobileTimer.app
News 1 458 555 News.app
TV 1 82 692 AppleTV.app
Podcasts 1 207 692 Podcasts.app
AppStore 1 332 692 AppStore.app
Maps 1 458 692 Maps.app
Health 1 82 830 Health.app
Wallet 1 207 830 Passbook.app
Settings 1 332 830 Preferences.app
Weather 2 82 145 Weather.app
Stocks 2 207 145 Stocks.app
FindMy 2 332 145 FindMy.app
Home 2 458 145 Home.app
Books 2 82 282 Books.app
iTunesStore 2 207 282 MobileStore.app
Fitness 2 332 282 Fitness.app
Contacts 2 458 282 Contacts.app
Translate 2 82 419 SequoiaTranslator.app
Files 2 207 419 Files.app
Shortcuts 2 332 419 Shortcuts.app
Freeform 2 82 556 Freeform.app
Tips 2 207 556 Tips.app
VoiceMemos f 145 430 VoiceMemos.app
Compass f 270 430 Compass.app
Measure f 395 430 Measure.app
Magnifier f 145 567 Magnifier.app
Calculator f 270 567 Calculator.app
Phone d 82 1088 MobilePhone.app
Safari d 207 1088 MobileSafari.app
Messages d 332 1088 MobileSMS.app
Music d 458 1088 Music.app"
H() { $SS call kcmd "$@" >/dev/null 2>&1; }
tap() { H touch tap $(($1 * 2)) $(($2 * 2)); }
crashes() { grep -a -c 'CRASH' "$LOG" 2>/dev/null; }
page=1
printf 'app\tprocess\tpid\tcrashes\tscreenshot\n' > "$O/apps.tsv"
while read -r name p x y frag; do
  [ "$ONLY" != "  " ] && [[ "$ONLY" != *" $name "* ]] && continue
  want=$p; [ "$p" = f ] || [ "$p" = d ] && want=2
  [ $page = 1 ] && [ $want = 2 ] && { H touch swipe left; sleep 3; page=2; }
  [ $page = 2 ] && [ $want = 1 ] && { H touch swipe right; sleep 3; page=1; }
  c0=$(crashes)
  [ "$p" = f ] && { tap 458 419; sleep 3; }
  tap "$x" "$y"; sleep "$WAIT"
  $SS call screenshot "$O/app-$name.png" >/dev/null 2>&1
  pid=$($SS run /bin/ps -A -o pid,comm 2>&1 | grep -F "/$frag/" | grep -v -E '/PlugIns/|\.appex' | head -1 | awk '{print $1}')
  new=$(grep -a 'CRASH' "$LOG" | tail -n +$((c0 + 1)) | awk '{print $5"("$6")"}' | sort | uniq -c | awk '{printf "%s%sx%s", s, $2, $1; s=","}')
  printf '%s\t%s\t%s\t%s\t%s\n' "$name" "${pid:+running}" "${pid:--}" "${new:--}" "app-$name.png" | tee -a "$O/apps.tsv"
  H touch swipe up; sleep 3
  [ "$p" = f ] && { H touch swipe up; sleep 2; }   # out of the folder
done <<< "$APPS"
