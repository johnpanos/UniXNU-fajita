#!/bin/bash
# uicache-style sideload of Hello.app (install tree under /private/var/containers/Bundle/Application, LaunchServices registration), launch, screenshot.
O=$1; S=$2; SS=$S/session.sh; R=$S/mig/s9; mkdir -p $R
$SS start $S/payload16-session.img > $R/start.out 2>&1; NAME=$(head -1 $R/start.out | cut -d: -f1)
trap '$SS end $NAME' EXIT
grep -q 'ready after' $R/start.out || exit 1
for t in instapp fsx; do $SS push $O/$t /private/var/ngpu/sbin/$t | grep -E 'put ok'; done
D=/private/var/containers/Bundle/Application/11111111-2222-3333-4444-555555555555/Hello.app
$SS run /usr/sbin/fsx mk $D 
$SS push $O/Hello.app/Info.plist $D/Info.plist 644 | grep put
$SS push $O/Hello.app/Hello $D/Hello 755 | grep put
$SS run /usr/sbin/instapp -root reg $D User | cut -c1-300
$SS run /usr/sbin/instapp list | grep -i hello
$SS run /usr/sbin/instapp launch org.unixnu.hello; sleep 6; $SS call screenshot $R/hello.png
$SS run ps -A | grep -i hello | cut -c1-120
