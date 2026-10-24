#!/bin/bash
# One short phone session: push the tools, sideload Hello.app (MobileInstallationInstall, then LaunchServices), launch it, screenshot, end.
# usage: session-sideload.sh OUTDIR(from build.sh) SCRATCH(session.sh dir)
O=$1; S=$2; SS=$S/session.sh; R=$S/mig/sl; mkdir -p $R
$SS start $S/payload16-session.img > $R/start.out 2>&1; NAME=$(head -1 $R/start.out | cut -d: -f1)
trap '$SS end $NAME' EXIT
grep -q 'ready after' $R/start.out || exit 1
for t in instapp fsx; do $SS push $O/$t /private/var/ngpu/sbin/$t | grep -E 'put ok'; done
D=/private/var/mobile/Media/PublicStaging
$SS run /usr/sbin/fsx mk $D/Hello.app
$SS push $O/Hello.app/Info.plist $D/Hello.app/Info.plist 644 | grep put
$SS push $O/Hello.app/Hello $D/Hello.app/Hello 755 | grep put
T=90 $SS run /usr/sbin/instapp mi $D/Hello.app | tee $R/mi.txt
$SS run /usr/sbin/instapp list | grep -i -E 'hello|Preferences'
$SS run /usr/sbin/instapp launch org.unixnu.hello; sleep 6; $SS call screenshot $R/hello.png
