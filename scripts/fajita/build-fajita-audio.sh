#!/bin/bash
# scripts/fajita/build-fajita-audio.sh [OUT_DIR]: build the CoreAudio HAL plug-in FajitaAudio.driver (package/fajita-audio: the Taptic
# "Actuator" iOS plays its haptics on, with its silent Speaker/Hawking companions; NOTES-drivers.md) on Linux, as build-usbdbg-agent.sh
# builds the debug tools: clang for arm64-apple-ios16.0 against .cache/apple-sdk through the scripts/gpu-local stand-ins, a bundle
# (MH_BUNDLE) ad-hoc signed with 4 KiB pages. OUT_DIR (default .cache/fajita-out/hal) gets FajitaAudio.driver/{Info.plist,FajitaAudio};
# scripts/fajita/build.sh puts it into the add-on as hal/ (the kernel mounts /private/var/ngpu/hal over /Library/Audio/Plug-Ins/HAL
# with fajita-haptics>=3).
set -eo pipefail
BOARD=$(cd "$(dirname "$0")/../.." && pwd)
C=$BOARD/.cache
OUT=${1:-${FAJITA_OUT:-$C/fajita-out}/hal}
export GPU_LOCAL_SDK=$C/apple-sdk/iPhoneOS.sdk GPU_LOCAL_CLANG_RT=$C/apple-sdk/toolchain/clang-rt/libclang_rt.ios.a
export GPU_LOCAL_RCODESIGN=$(ls "$C"/toolchain/apple-codesign-*/rcodesign 2>/dev/null | sort -V | tail -1)
[ -d "$GPU_LOCAL_SDK" ] || { echo "no SDK at $GPU_LOCAL_SDK (scripts/build-gpu-plugin-local.sh --fetch-sdk)"; exit 1; }
[ -x "$GPU_LOCAL_RCODESIGN" ] || { echo "no rcodesign under $C/toolchain (scripts/build-gpu-plugin-local.sh installs it)"; exit 1; }
export PATH=$BOARD/scripts/gpu-local/bin:$PATH SDKROOT=$GPU_LOCAL_SDK
B=$OUT/FajitaAudio.driver
rm -rf "$B"; mkdir -p "$B"
clang -target arm64-apple-ios16.0 -isysroot "$GPU_LOCAL_SDK" -O2 -Wall -Wextra -Werror -bundle \
  -framework CoreFoundation "$BOARD/package/fajita-audio/FajitaAudio.c" -o "$B/FajitaAudio"
cp "$BOARD/package/fajita-audio/Info.plist" "$B/Info.plist"
codesign --force --sign - --pagesize 4096 --identifier org.blu-sharky.FajitaAudio "$B"
codesign --verify --strict "$B"
echo "built $B"
