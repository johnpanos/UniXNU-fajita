#!/bin/bash
# scripts/fajita/build.sh [--test] [--skip-kernel] [--skip-plugin] [KERNEL_ARG...]
#
# One command for the current best iOS 16 build of the OnePlus 6T (fajita): XNU kernel, GPU plugin, boot payload and boot image,
# all from this tree (ios16-port) and the UniXNU stack commit it pins (ios16/gpu-plugin.ref). Then boot it with
# scripts/fajita/boot.sh. Output: $FAJITA_OUT (default .cache/fajita-out): payload.img, boot.img, metal.ngpu, build.txt.
#
#   --test         unattended test settings: the phone resets itself (sheng-autoreset=600 or $FAJITA_AUTORESET, hardware stop 90 s later). Without it the
#                  build is for using the phone: no automatic reset (the APSS watchdog still recovers a hard hang).
#   --skip-kernel  reuse the last kernel; --skip-plugin reuse the last plugin (both are rebuilt incrementally anyway).
#   KERNEL_ARG     extra boot arguments, e.g. fajita-brightness=1.
#   --candidate    instead: rebuild the storage candidate input (fajita_ios16_candidate_defconfig, the prelinked kernel the storage kit is
#                  qualified against) into .cache/fajita-inputs/storage-candidate/. Needed once, or when the stack's kit ABI changes.
#
# The stack lives in its own worktree of ~/src/UniXNU at the pinned commit ($FAJITA_STACK, default ~/src/UniXNU-fajita-stack); the
# script creates it and moves it to the pin, so nobody needs to manage it by hand. Inputs that are not in git (Apple SDK, firmware,
# the boot image, the data seed, trust caches) live in .cache (gitignored); see "Inputs" in AGENTS.md.
set -eo pipefail
BOARD=$(cd "$(dirname "$0")/../.." && pwd)
# One build at a time: two builds share the stack checkout and the output, and break each other's.
mkdir -p "$BOARD/.cache"
[ -n "$FAJITA_BUILD_LOCKED" ] || { export FAJITA_BUILD_LOCKED=1
  flock -n "$BOARD/.cache/build.lock" true || echo "another build is running; waiting for it ..."
  exec flock "$BOARD/.cache/build.lock" "$0" "$@"; }
UNIXNU_REPO=${UNIXNU_REPO:-$HOME/src/UniXNU}
STACK=${FAJITA_STACK:-$HOME/src/UniXNU-fajita-stack}
OUT=${FAJITA_OUT:-$BOARD/.cache/fajita-out}
IN=$BOARD/.cache/fajita-inputs
TEST=0; SKIP_K=0; SKIP_P=0; CAND=0; EXTRA=()
for a in "$@"; do case $a in
  --test) TEST=1 ;; --candidate) CAND=1 ;; --skip-kernel) SKIP_K=1 ;; --skip-plugin) SKIP_P=1 ;;
  -h|--help) sed -n 2,17p "$0"; exit 0 ;;
  *) EXTRA+=("$a") ;;
esac; done
mkdir -p "$OUT"; LOG=$OUT/build.txt; : > "$LOG"
say() { echo "[$(date +%T)] $*" | tee -a "$LOG"; }
NEED="storage-candidate/kernel.macho storage-candidate/kernel.json"; [ $CAND = 1 ] && NEED=
for f in boot.img $NEED metal-base.ngpu platform-seed fjdbg-agent; do
  [ -e "$IN/$f" ] || { echo "missing input $IN/$f (see AGENTS.md, Inputs)"; exit 1; }
done

# 1. The UniXNU stack at the pinned commit (one tree for the kernel and the plugin).
REF=$(head -1 "$BOARD/ios16/gpu-plugin.ref")
git -C "$UNIXNU_REPO" fetch -q fork 2>/dev/null || true
if [ ! -e "$STACK/.git" ]; then git -C "$UNIXNU_REPO" worktree add -q --detach "$STACK" "$REF"; fi
if [ "$(git -C "$STACK" rev-parse HEAD)" != "$(git -C "$UNIXNU_REPO" rev-parse "$REF")" ]; then
  [ -z "$(git -C "$STACK" status --porcelain --ignore-submodules=all | grep -v '^??')" ] || { echo "$STACK has uncommitted changes; refusing to move it"; exit 1; }
  git -C "$STACK" checkout -q --detach "$REF"
fi
# Submodule pins that are not upstream yet live on the johnpanos forks (branch fajita-a630): fetch any missing pin from there.
subs() {
  local top=$1
  git -C "$top" submodule update --init -q 2>/dev/null && return 0
  git -C "$top" config -f .gitmodules --get-regexp '\.path$' | while read -r _ p; do
    c=$(git -C "$top" ls-tree HEAD "$p" | awk '{print $3}')
    git -C "$top/$p" cat-file -e "$c^{commit}" 2>/dev/null && continue
    git -C "$top" submodule init -q -- "$p"; git -C "$top" submodule update -q -- "$p" 2>/dev/null && continue
    n=$(basename "$(git -C "$top" config -f .gitmodules "submodule.$p.url")" .git)
    n=$(gh api "repos/UniXNU/$n" --jq .name 2>/dev/null || echo "$n")   # upstream renames (NativePrelink -> XnuPrelink)
    [ -e "$top/$p/.git" ] || { say "submodule $p did not clone"; return 1; }
    say "fetching $p $c from johnpanos/$n"
    git -C "$top/$p" fetch -q "git@github.com:johnpanos/$n.git" "$c"
  done
  git -C "$top" submodule update --init -q
}
subs "$STACK"; subs "$BOARD"
say "board $(git -C "$BOARD" rev-parse --short HEAD) ($(git -C "$BOARD" rev-parse --abbrev-ref HEAD)), stack $(git -C "$STACK" rev-parse --short HEAD)"

export UNIXNU_OUT=$OUT/unixnu UNIXNU_UEFI=${UNIXNU_UEFI:-$HOME/src/Mu-Silicium}
export UNIXNU_XNU_CLANG=${UNIXNU_XNU_CLANG:-$HOME/unixnu-toolchain/clang/bin/clang} UNIXNU_LD64=${UNIXNU_LD64:-$HOME/unixnu-toolchain/ld64/bin/arm64-apple-darwin-ld}
# The cctools/ld64 build carries a RUNPATH into the scratch directory it was built in; its libdispatch/libBlocksRuntime sit in ../lib64.
export LD_LIBRARY_PATH=$(dirname "$UNIXNU_LD64")/../lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export UNIXNU_IIG_OUTPUTS=$BOARD/.cache/iig-outputs-20H392 FAJITA_USBDBG_AGENT=$IN/fjdbg-agent
export PYTHONPATH=$STACK/scripts FAJITA_STORAGE_CANDIDATE=$IN/storage-candidate/kernel.macho FAJITA_VERIFIED_ROOT_CACHE=$BOARD/.cache/verified-root
D16=$BOARD/ios16
cd "$STACK"

if [ $CAND = 1 ]; then
  say "storage candidate (fajita_ios16_candidate_defconfig) ..."
  export UNIXNU_OUT=$OUT/candidate; unset FAJITA_STORAGE_CANDIDATE
  ./unixnu defconfig fajita_ios16_candidate_defconfig -D "$D16" > /dev/null || { say "CANDIDATE DEFCONFIG FAILED"; exit 1; }
  ./unixnu build kernel -D "$D16" > "$OUT/candidate.log" 2>&1 || { say "CANDIDATE BUILD FAILED"; tail -8 "$OUT/candidate.log"; exit 1; }
  mkdir -p "$IN/storage-candidate"; cp "$UNIXNU_OUT/fajita-ios16/kernel/kernel.macho" "$UNIXNU_OUT/fajita-ios16/kernel/kernel.json" "$IN/storage-candidate/"
  say "storage candidate: $(sha256sum "$IN/storage-candidate/kernel.macho" | cut -c1-16)"; exit 0
fi

# 2. Kernel.
if [ $SKIP_K = 0 ]; then
  say "kernel (fajita_ios16_gpu_defconfig) ..."
  ./unixnu defconfig fajita_ios16_gpu_defconfig -D "$D16" > /dev/null
  ./unixnu build kernel -D "$D16" > "$OUT/kernel.log" 2>&1 || { say "KERNEL BUILD FAILED"; tail -8 "$OUT/kernel.log"; exit 1; }
  say "$(tail -1 "$OUT/kernel.log")"
fi

# 3. GPU plugin (Turnip, metal2vulkan, NativeMetal), packed into the boot add-on with the plugin's settings.
if [ $SKIP_P = 0 ] || [ ! -e "$OUT/metal.ngpu" ]; then
  say "GPU plugin ..."
  "$BOARD/scripts/build-gpu-plugin-local.sh" --unixnu "$STACK" --pack "$OUT/metal.ngpu" --base "$IN/metal-base.ngpu" > "$OUT/plugin.log" 2>&1 \
    || { say "PLUGIN BUILD FAILED"; tail -8 "$OUT/plugin.log"; exit 1; }
  say "$(grep -a 'built\|packed' "$OUT/plugin.log" | tail -2 | tr '\n' ' ')"
fi

# 4. Payload: the current best boot arguments (GPU compositing, USB debug channel; touch, charging, keys, battery come from board_args).
ARGS=(usb-debug=3 fajita-gpu=11 fajita-gpu-delay=8 sheng-fb-gpu=1)
# FAJITA_AUTORESET (default 600 s): the test build's reset; the hardware stop follows 90 s later.
AR=${FAJITA_AUTORESET:-600}
[ $TEST = 1 ] && ARGS+=(sheng-autoreset=$AR wdstop=$((AR + 90)))
ARGS+=("${EXTRA[@]}")
KA=(); for a in "${ARGS[@]}"; do KA+=("--kernel-arg=$a"); done
say "payload: ${ARGS[*]}"
FAJITA_EARLY_WDOG=1 FAJITA_PANIC_RESET=${FAJITA_PANIC_RESET:-30} UNIXNU_UNIQUE_CHIP_ID=$(cat "$IN/platform-seed") \
  ./unixnu build payload -D "$D16" --uefi "$UNIXNU_UEFI" --trust-cache "$BOARD/.cache/trust/original-static-system.bundle" \
  --data-ramdisk "$BOARD/.cache/data-seed-ios16/data-macos.dbr" --gpu-addon "$OUT/metal.ngpu" "${KA[@]}" > "$OUT/payload.log" 2>&1 \
  || { say "PAYLOAD BUILD FAILED"; tail -8 "$OUT/payload.log"; exit 1; }
cp "$UNIXNU_OUT/fajita-ios16/payload/payload.img" "$OUT/payload.img"
cp "$IN/boot.img" "$OUT/boot.img"
say "done: $OUT/payload.img + $OUT/boot.img -> scripts/fajita/boot.sh"
