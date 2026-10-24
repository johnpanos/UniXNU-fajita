#!/bin/bash
# build-gpu-plugin-local.sh: the "GPU plugin (iOS 16)" CI build (.github/workflows/gpu-plugin.yml) on this Linux host.
#
# Runs UniXNU's own build scripts (build-turnip-darwin.py --target ios, build-adreno-metal-plugin.py with the bridge) unchanged, with the
# Xcode tools replaced by scripts/gpu-local/tool.py: LLVM clang + ld64.lld against the CI's own iPhoneOS 18.5 SDK, rcodesign for the ad-hoc
# 4 KiB signatures, and the scanout metallib taken from a CI artifact by the .metal source's hash or, for a shader the cache lacks,
# compiled with Apple's Metal Developer Tools for Windows under Wine (--setup-metal-win).
# The output directory has the CI artifact's layout (NativeAdrenoMetal.bundle, scanout_scale.metallib, adrenoprobed, plists, trust.json,
# sources.txt, codesign.txt), so plugin-addon.sh / make-fajita-gpu-addon.py take it as they take a downloaded artifact.
#
# Incremental: Turnip is rebuilt only when its CI cache key changes (Mesa pin, patches, the KMD ABI header, the Turnip script); the bridge
# is cargo-incremental; the plugin's .mm sources compile one per process in parallel through ccache. A NativeMetal-only change costs the
# frontend .mm compile plus the link and signing.
#
# Usage:
#   scripts/build-gpu-plugin-local.sh [--unixnu DIR] [--out DIR] [--pack OUT.ngpu [--base BASE.ngpu]] [--tools]
#     --dev builds a tree whose inputs are dirty or whose submodules are off their pins (work in progress); sources.txt says so
#     --tools also builds the "GPU userland tools" CI artifact (gpu-tools.yml: ngpu-probe, ngpu-metal, ngpu-vk) into <out>-tools
#   scripts/build-gpu-plugin-local.sh --fetch-sdk [RUN_ID]     # the encrypted SDK export (apple-sdk-export.yml) -> .cache/apple-sdk
#   scripts/build-gpu-plugin-local.sh --seed-metallib RUN_ID   # a "GPU plugin (iOS 16)" run's metallib -> .cache/gpu-local/metallib
#   scripts/build-gpu-plugin-local.sh --setup-metal-win        # install the newest .cache/metal-win/Metal_Developer_Tools*Windows.exe
#     (Apple ID download from developer.apple.com) into the Wine prefix .cache/metal-win/prefix-<version>; needs wine
# Defaults: --unixnu $UNIXNU or /home/john/src/UniXNU-ios16-stack-gpuuser, --out <UNIXNU>/build/gpu-plugin-local.
# Never commit .cache/apple-sdk or .cache/metal-win: they are Apple's SDK and tools (gitignored).
set -euo pipefail
BOARD=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
UNIXNU=${UNIXNU:-/home/john/src/UniXNU-ios16-stack-gpuuser}
OUT= PACK= BASE= FETCH_SDK= SEED= TOOLS= SETUP_MW= DEV=
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
    --dev) DEV=1; shift ;;
    --setup-metal-win) SETUP_MW=1; shift ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
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
# The Metal Developer Tools for Windows: the newest installed prefix (3.1 is metalfe-31001.660; Xcode 16.4 is 32023.620).
METAL_WIN=$CACHE/metal-win
METAL_WINEPREFIX=${GPU_LOCAL_METAL_WINEPREFIX:-$(ls -d "$METAL_WIN"/prefix-* 2>/dev/null | sort -V | tail -1 || true)}
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
if [ -n "$SETUP_MW" ]; then
  command -v wine >/dev/null || { echo 'no wine: sudo dnf install wine' >&2; exit 1; }
  INST=$(ls "$METAL_WIN"/Metal_Developer_Tools*Windows.exe 2>/dev/null | sort -V | tail -1)
  [ -n "$INST" ] || { echo "no installer in $METAL_WIN: download Metal Developer Tools for Windows from developer.apple.com (Apple ID)" >&2; exit 1; }
  V=$(basename "$INST" | sed -E 's/^Metal_Developer_Tools([0-9.]+)Windows\.exe$/\1/')
  export WINEPREFIX=$METAL_WIN/prefix-$V WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml='
  wineboot -i >/dev/null 2>&1; wine "$INST" /S '/D=C:\metal' >/dev/null 2>&1
  wine 'C:\metal\ios\bin\metal.exe' -v 2>&1 | head -1; echo "-> $WINEPREFIX"; exit 0
fi
if [ -n "$SEED" ]; then
  tmp=$(mktemp -d "$CACHE/seed.XXXXXX"); trap 'rm -rf "$tmp"' EXIT
  gh run download "$SEED" -R $REPO -n gpu-plugin -D "$tmp"
  NM=$(awk '/drivers\/NativeMetal/{print $1; exit}' "$tmp/sources.txt"); NM=${NM#[-+ ]}
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
# The tree must be what git says it is. A file copy of a worktree keeps the original's .git file, so git there reports the ORIGINAL's HEAD,
# index and submodule checkouts while the build reads the copy's files (int: a copy of the gpuuser stack built translator d489756 while git
# showed fa4fdf0). Refused always. Dirty inputs (scripts/, patches/) or plugin submodules off their pin or modified are refused unless --dev.
SUBS="drivers/NativeMetal drivers/NativeAdreno drivers/NativeMobileDisplay"
tree_problems() {
  local gd wt sm pin have
  if [ -f .git ]; then
    gd=$(sed -n 's/^gitdir: //p' .git); case $gd in /*) ;; *) gd=$PWD/$gd ;; esac
    wt=$(cat "$gd/gitdir" 2>/dev/null || true)
    [ "$(realpath -m "${wt%/.git}")" = "$(realpath "$PWD")" ] || { echo "FATAL: $PWD/.git points at $gd, the worktree of ${wt%/.git}: this tree is a file copy (make it with git worktree add)"; return; }
  fi
  for sm in $SUBS; do
    [ -e "$sm/.git" ] || { echo "FATAL: $sm is not checked out"; continue; }
    wt=$(git -C "$sm" rev-parse --show-toplevel 2>/dev/null || true)
    [ "$(realpath -m "$wt")" = "$(realpath "$sm")" ] || { echo "FATAL: $sm's git metadata belongs to $wt: file copy"; continue; }
    pin=$(git ls-tree HEAD "$sm" | awk '{print $3}'); have=$(git -C "$sm" rev-parse HEAD)
    [ "$pin" = "$have" ] || echo "dirty: $sm at ${have:0:12}, pinned ${pin:0:12}"
    [ -z "$(git -C "$sm" status --porcelain --untracked-files=no)" ] || echo "dirty: $sm has uncommitted changes"
  done
  git status --porcelain --untracked-files=no -- scripts patches | sed 's/^/dirty: input /'
}
PROBLEMS=$(tree_problems)
if [ -n "$PROBLEMS" ]; then
  echo "$PROBLEMS" >&2
  if grep -q '^FATAL' <<<"$PROBLEMS"; then echo "refused: $UNIXNU is not a real checkout" >&2; exit 1; fi
  [ -n "$DEV" ] || { echo "refused: $UNIXNU differs from its commit (above); commit it, or pass --dev for a work-in-progress build" >&2; exit 1; }
fi
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
export GPU_LOCAL_METALLIB_CACHE=$METALLIB_CACHE GPU_LOCAL_OBJ=$UNIXNU/build/gpu-local-obj GPU_LOCAL_METAL_WINEPREFIX=$METAL_WINEPREFIX
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
( git rev-parse HEAD; git submodule status drivers/NativeMetal drivers/NativeAdreno; echo "local $(hostname) $(date -u +%FT%TZ)"
  echo "built translator $M2V mesa $MESA"
  for sm in $SUBS; do echo "built $sm $(git -C "$sm" rev-parse HEAD)"; done
  [ -z "$PROBLEMS" ] || { echo "DEV BUILD (--dev): not the commit above"; echo "$PROBLEMS"; } ) > "$OUT/sources.txt"
codesign -dvvv "$OUT/NativeAdrenoMetal.bundle" 2>&1 | grep -E 'CDHash|Page size|Identifier' > "$OUT/codesign.txt"
stamp "built $OUT ($(grep -o '"cdhash": "[0-9a-f]*' "$OUT/trust.json" | cut -c12-))"
if [ -n "$PACK" ]; then
  S=/tmp/claude-1000/-home-john-src-UniXNU/1375c105-6991-4ff3-9a85-56683ac7fae0/scratchpad/gpuuser
  # Files under /private/var/ngpu persist on the data partition across boots, so the pack states the render settings explicitly: TU_DEBUG
  # sysmem,nocb (the per-boot A/B: gmem, Turnip's default and sysmem,nocb within 1 fps; sysmem the plugin's own default) and no gpu-env
  # diagnostics. A later addon layered on this one replaces either.
  printf 'sysmem,nocb\n' > "$OUT/tu-debug"; printf '# no diagnostics\n' > "$OUT/gpu-env"
  python3 -I "$BOARD/scripts/make-fajita-gpu-addon.py" "$PACK" --base "${BASE:-$S/metal.ngpu}" --tree "addons/NativeAdrenoMetal.bundle=$OUT/NativeAdrenoMetal.bundle" \
    --file "tu-debug=$OUT/tu-debug:644" --file "gpu-env=$OUT/gpu-env:644"
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
