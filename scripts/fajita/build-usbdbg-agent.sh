#!/bin/bash
# scripts/fajita/build-usbdbg-agent.sh [OUT]: build the userland end of the USB debug channel (package/usb-debug-tools/fjdbg-agent.c)
# on Linux, as .github/workflows/usb-debug-agent.yml does on macOS: clang for arm64-apple-ios16.0 against the CI's iPhoneOS SDK
# (.cache/apple-sdk) and an ad-hoc signature with its entitlements, through the scripts/gpu-local stand-ins. Default OUT is the
# build input .cache/fajita-inputs/fjdbg-agent (the previous one is kept as fjdbg-agent.prev); scripts/fajita/build.sh embeds it.
set -eo pipefail
BOARD=$(cd "$(dirname "$0")/../.." && pwd)
C=$BOARD/.cache
OUT=${1:-$C/fajita-inputs/fjdbg-agent}
export GPU_LOCAL_SDK=$C/apple-sdk/iPhoneOS.sdk GPU_LOCAL_CLANG_RT=$C/apple-sdk/toolchain/clang-rt/libclang_rt.ios.a
export GPU_LOCAL_RCODESIGN=$(ls "$C"/toolchain/apple-codesign-*/rcodesign 2>/dev/null | sort -V | tail -1)
[ -d "$GPU_LOCAL_SDK" ] || { echo "no SDK at $GPU_LOCAL_SDK (scripts/build-gpu-plugin-local.sh --fetch-sdk)"; exit 1; }
[ -x "$GPU_LOCAL_RCODESIGN" ] || { echo "no rcodesign under $C/toolchain (scripts/build-gpu-plugin-local.sh installs it)"; exit 1; }
export PATH=$BOARD/scripts/gpu-local/bin:$PATH SDKROOT=$GPU_LOCAL_SDK
T=$(mktemp -d "$C/agent-build.XXXX"); trap 'rm -rf "$T"' EXIT
clang -target arm64-apple-ios16.0 -isysroot "$GPU_LOCAL_SDK" -O2 -Wall -Wextra -Werror \
  "$BOARD/package/usb-debug-tools/fjdbg-agent.c" -o "$T/fjdbg-agent"
codesign --force --sign - --pagesize 4096 --identifier org.blu-sharky.fjdbg-agent \
  --entitlements "$BOARD/package/usb-debug-tools/fjdbg-agent-entitlements.plist" "$T/fjdbg-agent"
codesign --verify --strict "$T/fjdbg-agent"
[ -f "$OUT" ] && [ "$OUT" = "$C/fajita-inputs/fjdbg-agent" ] && cp "$OUT" "$OUT.prev"
cp "$T/fjdbg-agent" "$OUT"
# fjnet: the network commands iOS lacks (push to the phone with session.sh push; not embedded in the payload)
clang -target arm64-apple-ios16.0 -isysroot "$GPU_LOCAL_SDK" -O2 -Wall -Wextra -Werror \
  -framework CoreFoundation -framework CFNetwork "$BOARD/package/usb-debug-tools/fjnet.c" -o "$T/fjnet"
codesign --force --sign - --pagesize 4096 --identifier org.blu-sharky.fjnet \
  --entitlements "$BOARD/package/usb-debug-tools/fjdbg-agent-entitlements.plist" "$T/fjnet"
cp "$T/fjnet" "$(dirname "$OUT")/fjnet"
# fjbundle: CFBundle probe for the WebKit XPC services (push with session.sh push; not embedded in the payload)
clang -target arm64-apple-ios16.0 -isysroot "$GPU_LOCAL_SDK" -O2 -Wall -Wextra -Werror \
  -framework CoreFoundation "$BOARD/package/usb-debug-tools/fjbundle.c" -o "$T/fjbundle"
codesign --force --sign - --pagesize 4096 --identifier org.blu-sharky.fjbundle \
  --entitlements "$BOARD/package/usb-debug-tools/fjbundle-entitlements.plist" "$T/fjbundle"
cp "$T/fjbundle" "$(dirname "$OUT")/fjbundle"
# fjsurface: every process's IOSurfaces (size, format, purgeable state, pixel census, raw dump); push with session.sh push
clang -target arm64-apple-ios16.0 -isysroot "$GPU_LOCAL_SDK" -O2 -Wall -Wextra -Werror \
  -framework CoreFoundation -framework CoreGraphics -framework IOSurface "$BOARD/package/usb-debug-tools/fjsurface.c" -o "$T/fjsurface"
codesign --force --sign - --pagesize 4096 --identifier org.blu-sharky.fjsurface \
  --entitlements "$BOARD/package/usb-debug-tools/fjsurface-entitlements.plist" "$T/fjsurface"
cp "$T/fjsurface" "$(dirname "$OUT")/fjsurface"
codesign -dvvv "$OUT" 2>&1 | grep -E 'CDHash|Identifier' || true
echo "built $OUT"
