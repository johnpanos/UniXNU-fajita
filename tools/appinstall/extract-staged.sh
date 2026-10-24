#!/bin/bash
# extract-staged.sh OUTDIR [DMG]: extract /private/var/staged_system_apps from the iOS 16.7.16 (20H392) OS image of the IPSW cache.
# Defaults to .cache/ipsw/20H392/087-86698-044.dmg of the ios16 tree and REFUSES any image whose SystemVersion is not 16.x
# (the 21H461 iPadOS image holds iOS 17 apps that abort in dyld on this userland).  Needs dmg2img and apfswalk (build-apfswalk.sh).
set -e; H=$(cd "$(dirname "$0")" && pwd); O=${1:?outdir}
DMG=${2:-${IPSW_OS_DMG:-/home/john/src/UniXNU-fajita-ios16/.cache/ipsw/20H392/087-86698-044.dmg}}
WALK=${APFSWALK:-$H/apfswalk}; CAT=${APFSCAT:-apfscat}
[ -x "$WALK" ] || "$H/build-apfswalk.sh" "$WALK"
mkdir -p "$O"; RAW=$O/os.img; [ -f "$RAW" ] || dmg2img -i "$DMG" -o "$RAW" >/dev/null
OFF=$(python3 -I -c "d=open('$RAW','rb').read(64<<20);i=d.find(b'NXSB');print(i-32)")
ver=$("$WALK" "$RAW" "$OFF" 0 /System/Library/CoreServices "$O/cs" >/dev/null 2>&1; python3 -I -c "import plistlib;print(plistlib.load(open('$O/cs/SystemVersion.plist','rb'))['ProductVersion'])")
case $ver in 16.*) ;; *) echo "refusing: image is $ver, not iOS 16"; exit 1;; esac
rm -rf "$O/cs"; "$WALK" "$RAW" "$OFF" 0 /private/var/staged_system_apps "$O/staged"; echo "staged apps ($ver) in $O/staged"
