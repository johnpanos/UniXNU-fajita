#!/bin/bash
# build-gpu-plugin-local.sh: the "GPU plugin (iOS 16)" CI build (.github/workflows/gpu-plugin.yml) on this Linux host.
#
# Runs UniXNU's own build scripts (build-turnip-darwin.py --target ios, build-adreno-metal-plugin.py with the bridge) unchanged, with the
# Xcode tools replaced by scripts/gpu-local/tool.py: LLVM clang + ld64.lld against the CI's own iPhoneOS 18.5 SDK, rcodesign for the ad-hoc
# 4 KiB signatures, and the scanout metallib (Apple's Metal compiler is macOS-only) taken from a CI artifact by the .metal source's hash.
# The output directory has the CI artifact's layout (NativeAdrenoMetal.bundle, scanout_scale.metallib, adrenoprobed, plists, trust.json,
# sources.txt, codesign.txt), so plugin-addon.sh / make-fajita-gpu-addon.py take it as they take a downloaded artifact.
#
# Incremental: Turnip is rebuilt only when its CI cache key changes (Mesa pin, patches, the KMD ABI header, the Turnip script); the bridge
# is cargo-incremental; the plugin's .mm sources compile one per process in parallel through ccache. A NativeMetal-only change costs the
# frontend .mm compile plus the link and signing.
#
# Usage:
#   scripts/build-gpu-plugin-local.sh [--unixnu DIR] [--out DIR] [--pack OUT.ngpu [--base BASE.ngpu]] [--tools]
#     --tools also builds the "GPU userland tools" CI artifact (gpu-tools.yml: ngpu-probe, ngpu-metal, ngpu-vk) into <out>-tools
#   scripts/build-gpu-plugin-local.sh --fetch-sdk [RUN_ID]     # the encrypted SDK export (apple-sdk-export.yml) -> .cache/apple-sdk
#   scripts/build-gpu-plugin-local.sh --seed-metallib RUN_ID   # a "GPU plugin (iOS 16)" run's metallib -> .cache/gpu-local/metallib
# Defaults: --unixnu $UNIXNU or /home/john/src/UniXNU-ios16-stack-gpuuser, --out <UNIXNU>/build/gpu-plugin-local.
# Never commit .cache/apple-sdk: it is Apple's SDK (gitignored).
set -euo pipefail
BOARD=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UNIXNU=${UNIXNU:-/home/john/src/UniXNU-ios16-stack-gpuuser}
OUT= PACK= BASE= FETCH_SDK= SEED= TOOLS=
REPO=johnpanos/UniXNU-fajita
while [ $# -gt 0 ]; do
  case $1 in
    --unixnu) UNIXNU=$(cd "$2" && pwd); shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --pack) PACK=$2; shift 2 ;;
    --base) BASE=$2; shift 2 ;;
    --fetch-sdk) FETCH_SDK=1; shift; if [ $# -gt 0 ] && [[ $1 =~ ^[0-9]+$ ]]; then FETCH_SDK=$1; shift; fi ;;
    --seed-metallib) SEED=$2; shift 2 ;;
    --tools) TOOLS=1; shift ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
OUT=${OUT:-$UNIXNU/build/gpu-plugin-local}
CACHE=${GPU_LOCAL_CACHE:-$BOARD/.cache}   # SDK, rcodesign and metallib caches (gitignored); share one with GPU_LOCAL_CACHE
SDKDIR=$CACHE/apple-sdk
KEY=${SDK_TAR_KEY_FILE:-$HOME/.config/unixnu/sdk_tar_key}
RCODESIGN_VERSION=0.29.0
RCODESIGN=$CACHE/toolchain/apple-codesign-$RCODESIGN_VERSION-x86_64-unknown-linux-musl/rcodesign
METALLIB_CACHE=$CACHE/gpu-local/metallib
t0=$(date +%s.%N)
stamp() { printf '[%6.1fs] %s\n' "$(echo "$(date +%s.%N) - $t0" | bc)" "$*"; }

# ---- one-time inputs --------------------------------------------------------------------------------------------------------------
if [ -n "$FETCH_SDK" ]; then
  [ -s "$KEY" ] || { echo "no SDK key at $KEY (the repo secret SDK_TAR_KEY)" >&2; exit 1; }
  RUN=$FETCH_SDK
  if [ "$RUN" = 1 ]; then
    gh workflow run apple-sdk-export.yml -R $REPO --ref "$(git -C "$BOARD" rev-parse --abbrev-ref HEAD)"; sleep 8
    RUN=$(gh run list -R $REPO --workflow apple-sdk-export.yml -L 1 --json databaseId -q '.[0].databaseId')
    gh run watch "$RUN" -R $REPO --exit-status >/dev/null
  fi
  tmp=$(mktemp -d "$CACHE/sdk-dl.XXXXXX"); trap 'rm -rf "$tmp"' EXIT
  gh run download "$RUN" -R $REPO -n apple-sdk-enc -D "$tmp"
  rm -rf "$SDKDIR"; mkdir -p "$SDKDIR"
  openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -pass "file:$KEY" -in "$tmp/sdk.tgz.enc" | tar -C "$SDKDIR" --warning=no-unknown-keyword -xzf -
  ID=$(gh api "repos/$REPO/actions/runs/$RUN/artifacts" -q '.artifacts[0].id'); gh api -X DELETE "repos/$REPO/actions/artifacts/$ID"
  echo "SDK: $(sed -n 2,3p "$SDKDIR/versions.txt" | tr '\n' ' ') -> $SDKDIR (artifact deleted)"; exit 0
fi
if [ -n "$SEED" ]; then
  tmp=$(mktemp -d "$CACHE/seed.XXXXXX"); trap 'rm -rf "$tmp"' EXIT
  gh run download "$SEED" -R $REPO -n gpu-plugin -D "$tmp"
  NM=$(awk '/drivers\/NativeMetal/{print $1}' "$tmp/sources.txt"); NM=${NM#[-+ ]}
  git -C "$UNIXNU/drivers/NativeMetal" cat-file -e "$NM" 2>/dev/null || git -C "$UNIXNU/drivers/NativeMetal" fetch -q origin "$NM"
  git -C "$UNIXNU/drivers/NativeMetal" show "$NM:shaders/scanout_scale.metal" > "$tmp/scanout_scale.metal"
  # The key is what tool.py's metal stand-in computes for the plugin script's call (build-adreno-metal-plugin.py, iOS 16 target).
  K=$(python3 -I -c "import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],'rb').read()+b'\0'+' '.join(sorted(['-std=metal3.0','-mios-version-min=16.0','-O2'])).encode()).hexdigest())" "$tmp/scanout_scale.metal")
  mkdir -p "$METALLIB_CACHE"; cp "$tmp/scanout_scale.metallib" "$METALLIB_CACHE/$K.metallib"
  echo "metallib of run $SEED (NativeMetal ${NM:0:7}) -> $METALLIB_CACHE/$K.metallib"; exit 0
fi
[ -d "$SDKDIR/iPhoneOS.sdk" ] || { echo "no SDK: run $0 --fetch-sdk first" >&2; exit 1; }
if [ ! -x "$RCODESIGN" ]; then
  mkdir -p "$CACHE/toolchain"
  gh release download "apple-codesign/$RCODESIGN_VERSION" -R indygreg/apple-platform-rs -p '*x86_64-unknown-linux-musl.tar.gz' -D "$CACHE/toolchain" --clobber
  tar -C "$CACHE/toolchain" -xzf "$CACHE/toolchain/apple-codesign-$RCODESIGN_VERSION-x86_64-unknown-linux-musl.tar.gz"
fi
rustup target list --toolchain 1.98.0 --installed 2>/dev/null | grep -q aarch64-apple-ios || rustup toolchain install 1.98.0 --profile minimal --target aarch64-apple-ios
for t in glslangValidator glslc spirv-val llvm-as ld64.lld bison ccache; do command -v $t >/dev/null || echo "warning: $t missing" >&2; done

cd "$UNIXNU"
# The pinned Mesa and translator sources, as the CI's "Pinned Mesa and translator" step makes them.
MESA=$(python3 -c "import json;print(json.load(open('scripts/data/adreno-compiler-inputs.json'))['mesa']['commit'])")
if [ ! -d .cache/mesa-work/.git ] || [ "$(git -C .cache/mesa-work rev-parse HEAD)" != "$MESA" ]; then
  stamp "Mesa $MESA + recorded patches"
  rm -rf .cache/mesa-work; git init -q .cache/mesa-work
  git -C .cache/mesa-work fetch -q --depth 1 https://gitlab.freedesktop.org/mesa/mesa "$MESA"
  git -C .cache/mesa-work checkout -q FETCH_HEAD
  for p in $(python3 -c "import json;print(' '.join(json.load(open('scripts/data/adreno-compiler-inputs.json'))['patches']))"); do
    git -C .cache/mesa-work apply "$PWD/$p"; done
fi
M2V=$(python3 -c "import json;print(json.load(open('scripts/data/gpu-metal-vulkan-inputs.json'))['translator']['commit'])")
if [ "$(git -C .cache/gpu-reference/metal2vulkan rev-parse HEAD 2>/dev/null)" != "$M2V" ]; then
  stamp "translator $M2V"
  rm -rf .cache/gpu-reference/metal2vulkan; git init -q .cache/gpu-reference/metal2vulkan
  git -C .cache/gpu-reference/metal2vulkan fetch -q --depth 1 \
    "$(python3 -c "import json;print(json.load(open('scripts/data/gpu-metal-vulkan-inputs.json'))['translator']['repository'])")" "$M2V"
  git -C .cache/gpu-reference/metal2vulkan checkout -q FETCH_HEAD
fi
if [ ! -x .cache/mesa-venv/bin/meson ]; then
  python3 -m venv .cache/mesa-venv && .cache/mesa-venv/bin/pip install -q -r scripts/requirements-adreno-compiler.txt
fi

export PATH=$BOARD/scripts/gpu-local/bin:$HOME/.cargo/bin:$PATH
export GPU_LOCAL_SDK=$SDKDIR/iPhoneOS.sdk GPU_LOCAL_CLANG_RT=$SDKDIR/toolchain/clang-rt/libclang_rt.ios.a GPU_LOCAL_RCODESIGN=$RCODESIGN
export GPU_LOCAL_METALLIB_CACHE=$METALLIB_CACHE GPU_LOCAL_OBJ=$UNIXNU/build/gpu-local-obj
export SDKROOT=$GPU_LOCAL_SDK CCACHE_BASEDIR=$UNIXNU CCACHE_NOHASHDIR=1
export UNIXNU_IOS_TARGET=16.0 UNIXNU_TURNIP_REPLAY=0 UNIXNU_SCALELIB=1
# The bridge's cc-built pieces (sha2 asm) target iOS through the same stand-ins.
export CC_aarch64_apple_ios=clang CXX_aarch64_apple_ios=clang++ AR_aarch64_apple_ios=ar

# Turnip: rebuilt only when the CI's mesa cache key changes (or the library is missing).
TKEY=$(cat scripts/data/adreno-compiler-inputs.json patches/mesa/*.patch drivers/NativeAdreno/NativeAdrenoABI.h scripts/build-turnip-darwin.py \
       | sha256sum | cut -c1-24)
# The stand-ins' flags are not in ninja's command lines: a different tool.py, compiler or SDK starts the Turnip tree afresh.
TCKEY=$( (cat "$BOARD/scripts/gpu-local/tool.py"; clang --version; cat "$GPU_LOCAL_SDK/SDKSettings.json") | sha256sum | cut -c1-24)
if [ "$(cat build/mesa-tu-ios/.gpu-local-toolchain 2>/dev/null)" != "$TCKEY" ]; then
  rm -rf build/mesa-tu-ios build/gpu-local-obj; mkdir -p build/mesa-tu-ios; echo "$TCKEY" > build/mesa-tu-ios/.gpu-local-toolchain
fi
LIB=build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib
if [ ! -f "$LIB" ] || [ "$(cat build/mesa-tu-ios/.gpu-local-key 2>/dev/null)" != "$TKEY" ]; then
  stamp "Turnip (darwin KMD, iOS 16)"
  python3 scripts/build-turnip-darwin.py --target ios || { tail -40 .cache/mesa-tu-ios-*.log; exit 1; }
  echo "$TKEY" > build/mesa-tu-ios/.gpu-local-key
else
  stamp "Turnip up to date ($TKEY)"
fi
stamp "bridge and plugin"
python3 scripts/build-adreno-metal-plugin.py

# Collect, as the CI's Collect step.
P=build/adreno-metal-plugin
rm -rf "$OUT"; mkdir -p "$OUT"
cp -R $P/NativeAdrenoMetal.bundle $P/scanout_scale.metallib $P/adrenoprobed $P/*.plist $P/trust.json "$OUT/"
( git rev-parse HEAD; git submodule status drivers/NativeMetal drivers/NativeAdreno; echo "local $(hostname) $(date -u +%FT%TZ)" ) > "$OUT/sources.txt"
codesign -dvvv "$OUT/NativeAdrenoMetal.bundle" 2>&1 | grep -E 'CDHash|Page size|Identifier' > "$OUT/codesign.txt"
stamp "built $OUT ($(grep -o '"cdhash": "[0-9a-f]*' "$OUT/trust.json" | cut -c12-))"
if [ -n "$PACK" ]; then
  S=/tmp/claude-1000/-home-john-src-UniXNU/1375c105-6991-4ff3-9a85-56683ac7fae0/scratchpad/gpuuser
  python3 -I "$BOARD/scripts/make-fajita-gpu-addon.py" "$PACK" --base "${BASE:-$S/metal.ngpu}" --tree "addons/NativeAdrenoMetal.bundle=$OUT/NativeAdrenoMetal.bundle"
  stamp "packed $PACK"
fi
if [ -n "$TOOLS" ]; then
  # As .github/workflows/gpu-tools.yml, with this UniXNU checkout's NativeAdreno/NativeMetal and the smoke header the plugin build made.
  TO=$OUT-tools VH=$CACHE/gpu-local/Vulkan-Headers-v1.4.309 GT=$BOARD/package/gpu-tools
  [ -d "$VH/include" ] || git clone -q --depth 1 -b v1.4.309 https://github.com/KhronosGroup/Vulkan-Headers "$VH"
  rm -rf "$TO"; mkdir -p "$TO"
  C=(xcrun --sdk iphoneos clang++ -std=c++17 -target arm64-apple-ios16.0 -isysroot "$GPU_LOCAL_SDK" -O2 -Wall -Wextra)
  "${C[@]}" -Werror -I "$GT/include" -I drivers/NativeAdreno "$GT/ngpu-probe.cpp" -framework IOKit -o "$TO/ngpu-probe" &
  "${C[@]}" -Werror -fobjc-arc -x objective-c++ "$GT/ngpu-metal.mm" -framework Foundation -framework Metal -o "$TO/ngpu-metal" &
  "${C[@]}" -Wno-unused-parameter -Wno-missing-field-initializers -I drivers/NativeMetal -I "$VH/include" -I build/turnip-smoke \
    "$GT/ngpu-vk.cpp" -o "$TO/ngpu-vk" &
  wait %1 && wait %2 && wait %3
  for t in ngpu-probe ngpu-metal ngpu-vk; do
    codesign --force --sign - --pagesize 4096 --identifier org.blu-sharky.$t --entitlements "$GT/ngpu-probe-entitlements.plist" "$TO/$t" 2>/dev/null
    codesign --verify --strict "$TO/$t"
  done
  codesign -dvvv "$TO/ngpu-probe" 2>&1 | grep -E 'CDHash|Page size|Identifier' > "$TO/codesign.txt"
  git -C drivers/NativeAdreno rev-parse HEAD > "$TO/native-adreno.txt"
  stamp "tools -> $TO"
fi
