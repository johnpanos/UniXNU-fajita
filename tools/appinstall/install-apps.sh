#!/bin/bash
# install-apps.sh SCRATCH OUTDIR STAGEDDIR [APP...]: on a RUNNING session (session.sh start ...) install apps under
# /private/var/containers/Bundle/Application/<UUID>/ with containermanagerd metadata and register them with LaunchServices.
#  - APP is a staged system bundle name (MobileCal, Weather, ...) taken from STAGEDDIR (apfswalk output of /private/var/staged_system_apps),
#    registered as System; "Hello" installs OUTDIR/Hello.app (build.sh) as User.
#  - UUIDs are deterministic: AAAAAAAA-0000-0000-0000-<crc of the app name>; the bundle id is read from the bundle's Info.plist.
# STAGEDDIR must come from the 16.7.16 OS image, NOT the 21H461 one (those apps need TipKit etc. and abort in dyld):
#   dmg2img -i .cache/ipsw/20H392/087-86698-044.dmg -o os16.img; apfswalk os16.img 17408 0 /private/var/staged_system_apps STAGEDDIR
# Needs OUTDIR/{instapp,fsx} already pushed to /private/var/ngpu/sbin (this script pushes them).
S=$1; O=$2; ST=$3; shift 3; SS=$S/session.sh; H=$(cd "$(dirname "$0")" && pwd); T=$(mktemp -d); B=/private/var/containers/Bundle/Application
for t in instapp fsx; do $SS push $O/$t /private/var/ngpu/sbin/$t | grep -q 'put ok' || { echo "push $t failed"; exit 1; }; done
for a in "$@"; do
  if [ $a = Hello ]; then src=$O/Hello.app; type=User; else src=$ST/$a.app; type=System; fi
  u=$(python3 -I -c "import zlib,sys;print('AAAAAAAA-0000-0000-0000-%012d'%zlib.crc32(sys.argv[1].encode()))" $a)
  id=$(python3 -I -c "import plistlib,sys;print(plistlib.load(open(sys.argv[1]+'/Info.plist','rb'))['CFBundleIdentifier'])" $src)
  (cd $(dirname $src) && python3 $H/pack.py $T/$a.bin $(basename $src))
  python3 $H/mkmeta.py $T/$a.meta $u $id
  $SS run /usr/sbin/fsx mk $B/$u > /dev/null
  $SS push $T/$a.meta $B/$u/.com.apple.mobile_container_manager.metadata.plist 644 | grep -E 'cannot' 
  $SS push $T/$a.bin /private/var/mobile/$a.bin 644 | grep -E 'cannot'
  $SS run /usr/sbin/fsx unpack /private/var/mobile/$a.bin $B/$u | grep unpacked
  $SS run /usr/sbin/instapp -root reg $B/$u/$a.app $type | grep register
  $SS run /usr/sbin/fsx rm /private/var/mobile/$a.bin > /dev/null
done
