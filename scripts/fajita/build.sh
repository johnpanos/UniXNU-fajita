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
# Heartbeat: the long steps write to their own logs, so every 30 s say which step is running and its last line (keeps bg.sh's
# stall detection and anyone tailing the output from mistaking a long compile for a hang).
( while kill -0 $$ 2>/dev/null; do sleep 30
    f=$(ls -t "$OUT"/kernel.log "$OUT"/plugin.log "$OUT"/payload.log "$OUT"/candidate.log 2>/dev/null | head -1)
    [ -n "$f" ] && [ $(( $(date +%s) - $(stat -c %Y "$f") )) -lt 120 ] && echo "[$(date +%T)] ... $(basename "$f" .log): $(tail -1 "$f" | cut -c1-140)"
  done ) &
HEARTBEAT=$!; trap 'kill $HEARTBEAT 2>/dev/null || true' EXIT   # || true: under set -e a failed kill (heartbeat already gone) made a good build exit 1
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
# A submodule checked out at another commit than its pin is built as it is, with a warning, never reset: `submodule update` used to
# revert a fresh checkout before its pin was committed, so a kernel built without the change (NativeAdreno HostDirty, 2026-10-25).
subs() {
  local top=$1 p c h paths=()
  while read -r _ p; do
    c=$(git -C "$top" ls-tree HEAD "$p" | awk '{print $3}')
    h=$(git -C "$top/$p" rev-parse -q --verify HEAD 2>/dev/null) || h=
    if [ -e "$top/$p/.git" ] && [ -n "$h" ] && [ "$h" != "$c" ]; then
      say "!! $(basename "$top")/$p is at ${h:0:9}, its pin is ${c:0:9}: building the checkout (commit the pin)"; continue
    fi
    paths+=("$p")
  done < <(git -C "$top" config -f .gitmodules --get-regexp '\.path$')
  [ ${#paths[@]} -gt 0 ] || return 0
  git -C "$top" submodule update --init -q -- "${paths[@]}" 2>/dev/null && return 0
  for p in "${paths[@]}"; do
    c=$(git -C "$top" ls-tree HEAD "$p" | awk '{print $3}')
    git -C "$top/$p" cat-file -e "$c^{commit}" 2>/dev/null && continue
    git -C "$top" submodule init -q -- "$p"; git -C "$top" submodule update -q -- "$p" 2>/dev/null && continue
    n=$(basename "$(git -C "$top" config -f .gitmodules "submodule.$p.url")" .git)
    n=$(gh api "repos/UniXNU/$n" --jq .name 2>/dev/null || echo "$n")   # upstream renames (NativePrelink -> XnuPrelink)
    [ -e "$top/$p/.git" ] || { say "submodule $p did not clone"; return 1; }
    say "fetching $p $c from johnpanos/$n"
    git -C "$top/$p" fetch -q "git@github.com:johnpanos/$n.git" "$c"
  done
  git -C "$top" submodule update --init -q -- "${paths[@]}"
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
  # NMB_TARGET_CPU=cortex-a75 (the bridge tuned for Kryo 385) is not the default: with it the render server's surface imports failed with
  # OutOfMemory(Device) (App Library backdrop black, 2026-10-24); fat LTO alone is correct and App Library scroll 32 -> 41 fps.
  NMB_TARGET_CPU=${NMB_TARGET_CPU-} "$BOARD/scripts/build-gpu-plugin-local.sh" --unixnu "$STACK" --pack "$OUT/metal.ngpu" --base "$IN/metal-base.ngpu" > "$OUT/plugin.log" 2>&1 \
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
# fajita-wifid=1: Apple's own wifid runs as the com.apple.wifid job (the USB debug agent forks off and execs it; NOTES-wifi.md).
# The addon carries the 20H392 binary from the extracted root (Apple material: .cache only, never committed); otherwise it is dropped.
REAL_WIFID=${FAJITA_REAL_WIFID:-$BOARD/.cache/mnt/os20H392/root/usr/sbin/wifid}
if printf '%s\n' "${ARGS[@]}" | grep -qx 'fajita-wifid=1'; then
  [ -r "$REAL_WIFID" ] || { say "fajita-wifid=1 needs $REAL_WIFID (the extracted 20H392 root)"; exit 1; }
  python3 "$BOARD/scripts/fajita/addon-put.py" "$OUT/metal.ngpu" --drop fjdbg/ "fjdbg/wifid.real=$REAL_WIFID:755" | sed 's/^/  /'
else
  python3 "$BOARD/scripts/fajita/addon-put.py" "$OUT/metal.ngpu" --drop fjdbg/ >/dev/null
fi
# fajita-ssh=1: the SSH dev mode (NOTES-devmode.md). The add-on tree from scripts/fajita/build-ssh.sh (.cache/fajita-inputs/ssh, rebuilt
# here when missing) goes in as /private/var/ngpu/ssh; the USB debug agent's supervisor starts dropbear from it.
SSH_IN=${FAJITA_SSH_IN:-$IN/ssh}
if printf '%s\n' "${ARGS[@]}" | grep -qx 'fajita-ssh=1'; then
  [ -x "$SSH_IN/bin/dropbear" ] || "$BOARD/scripts/fajita/build-ssh.sh" | sed 's/^/  /'
  [ -s "$SSH_IN/etc/authorized_keys" ] || say "!! fajita-ssh=1 with no authorized keys ($SSH_IN/etc/authorized_keys): nobody can log in"
  mapfile -t SSH_FILES < <(cd "$SSH_IN" && find . -type f | sed 's#^\./##' | sort)
  SSH_PUT=(); for f in "${SSH_FILES[@]}"; do m=644; [ -x "$SSH_IN/$f" ] && m=755; SSH_PUT+=("ssh/$f=$SSH_IN/$f:$m"); done
  python3 "$BOARD/scripts/fajita/addon-put.py" "$OUT/metal.ngpu" --drop ssh/ "${SSH_PUT[@]}" | sed 's/^/  /'
  say "ssh: ${#SSH_FILES[@]} files, $(grep -c . "$SSH_IN/etc/authorized_keys" 2>/dev/null || echo 0) authorized keys"
else
  python3 "$BOARD/scripts/fajita/addon-put.py" "$OUT/metal.ngpu" --drop ssh/ >/dev/null
fi
# The processes besides the render server that get their own GPU device (addons/NativeAdrenoMetal.clients, read by NativeAdrenoMetalPlugin.mm
# listedClient; a listed process takes one of the three 170 MiB client windows, NativeAdrenoPublish.cpp kClientWindows, while its device
# lives). WebKit draws its tiles through Metal: CoreGraphics renders the deep-colour backing stores (RGB10 'w30r' IOSurfaces, chosen for this
# P3 display) with Metal, in com.apple.WebKit.GPU (the GPU process, Safari's default) or in com.apple.WebKit.WebContent (GPU process off).
# Without a device there every tile stayed zero and Safari's page area was black (NOTES-wifi.md section 9).
# FAJITA_GPU_CLIENTS replaces the list (process names as getprogname() gives them).
GPU_CLIENTS=${FAJITA_GPU_CLIENTS:-ngpu-metal com.apple.WebKit.GPU com.apple.WebKit.WebContent}
printf '%s\n' $GPU_CLIENTS > "$OUT/gpu-clients"
python3 "$BOARD/scripts/fajita/addon-put.py" "$OUT/metal.ngpu" --drop addons/NativeAdrenoMetal.clients \
  "addons/NativeAdrenoMetal.clients=$OUT/gpu-clients:644" >/dev/null
say "gpu clients: $GPU_CLIENTS"
say "payload: ${ARGS[*]}"
FAJITA_EARLY_WDOG=1 FAJITA_PANIC_RESET=${FAJITA_PANIC_RESET:-30} UNIXNU_UNIQUE_CHIP_ID=$(cat "$IN/platform-seed") \
  ./unixnu build payload -D "$D16" --uefi "$UNIXNU_UEFI" --trust-cache "$BOARD/.cache/trust/original-static-system.bundle" \
  --data-ramdisk "$BOARD/.cache/data-seed-ios16/data-macos.dbr" --gpu-addon "$OUT/metal.ngpu" "${KA[@]}" > "$OUT/payload.log" 2>&1 \
  || { say "PAYLOAD BUILD FAILED"; tail -8 "$OUT/payload.log"; exit 1; }
cp "$UNIXNU_OUT/fajita-ios16/payload/payload.img" "$OUT/payload.img"
cp "$IN/boot.img" "$OUT/boot.img"
say "done: $OUT/payload.img + $OUT/boot.img -> scripts/fajita/boot.sh"
