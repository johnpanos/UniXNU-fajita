#!/bin/bash
# Build and ad-hoc sign the app-install tools on Linux: build.sh OUTDIR  (needs clang, ld64.lld, ipsw; SDK from the gpulocal cache).
# libSystem.tbd/libobjc.tbd stubs are taken from $SDK. Output: OUTDIR/{sysappmig,instapp,fsx,Hello.app}  (never commit these).
set -e; H=$(cd "$(dirname "$0")" && pwd); O=${1:?outdir}; mkdir -p "$O"
SDK=${SDK:-/home/john/src/UniXNU-fajita-gpulocal/.cache/apple-sdk/iPhoneOS.sdk}
CC=(clang -target arm64-apple-ios16.0 -isysroot "$SDK" -O1 -fuse-ld=lld)
sign() { rm -f "$O/$1.s"; ipsw macho sign -f --ad-hoc --id "$2" ${3:+--ent "$3"} ${4:+--ent-der "$4"} -o "$O/$1.s" "$O/$1" </dev/null >/dev/null 2>&1; mv "$O/$1.s" "$O/$1"; }
"${CC[@]}" "$H/sysappmig.c" -o "$O/sysappmig" "$SDK/usr/lib/libSystem.tbd" "$SDK/usr/lib/libobjc.tbd"; sign sysappmig com.apple.MobileInstallation.sysappmig "$H/entsA.plist" "$H/entsA.der"
"${CC[@]}" -fobjc-arc -include objc/message.h "$H/instapp.m" -o "$O/instapp" -framework Foundation; sign instapp com.apple.MobileInstallation.instapp "$H/entsA.plist" "$H/entsA.der"
"${CC[@]}" "$H/fsx.c" -o "$O/fsx" "$SDK/usr/lib/libSystem.tbd"; sign fsx com.apple.fsx
mkdir -p "$O/Hello.app"; "${CC[@]}" -fobjc-arc "$H/hello/main.m" -o "$O/Hello.app/Hello" -framework UIKit -framework Foundation
cp "$H/hello/Info.plist" "$O/Hello.app/"; sign Hello.app/Hello org.unixnu.hello "$H/hello/hello-ents.plist"
