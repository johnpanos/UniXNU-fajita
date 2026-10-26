#!/bin/bash
# scripts/fajita/build-ssh.sh [--clean]: build the phone's SSH dev mode userland (NOTES-devmode.md) on Linux, the way
# build-usbdbg-agent.sh builds the debug agent: clang for arm64-apple-ios16.0 against the iPhoneOS SDK (.cache/apple-sdk), ad-hoc
# signatures with the agent's entitlements, through the scripts/gpu-local stand-ins.
#
#   dropbear 2026.94   SSH server (key-only, port forwarding), dropbearkey, scp, dbclient  (package/ssh/dropbear-*.{h,patch})
#   OpenSSH 10.6p1     sftp-server only (--without-openssl)
#   bash 5.2.37        the login shell, readline against iOS's libncurses   (package/ssh/bash-fajita.patch)
#   toybox 0.8.15      ls, cp, grep, sed, tar, vi, ... (scripts/macos_miniconfig plus TOYBOX_ADD, minus TOYBOX_DROP)
#
# Sources are fetched into .cache/ssh-src (checked against the sha256 below) and built in .cache/ssh-build. Output: the add-on tree
# .cache/fajita-inputs/ssh/ (bin/ libexec/ etc/ share/terminfo/), which scripts/fajita/build.sh puts into the boot add-on as
# /private/var/ngpu/ssh when the build has fajita-ssh=1. etc/authorized_keys is taken from $FAJITA_SSH_KEYS (default: every
# ~/.ssh/*.pub of whoever builds). Nothing here is committed: binaries and tarballs stay in .cache.
set -eo pipefail
BOARD=$(cd "$(dirname "$0")/../.." && pwd)
C=$BOARD/.cache; P=$BOARD/package/ssh
SRC=$C/ssh-src; B=$C/ssh-build; OUT=${FAJITA_SSH_OUT:-$C/fajita-inputs/ssh}
[ "$1" = --clean ] && rm -rf "$B"
DROPBEAR=dropbear-2026.94 OPENSSH=openssh-10.6p1 BASH_V=bash-5.2.37 TOYBOX=toybox-0.8.15
declare -A URL=(
  [$DROPBEAR]=https://matt.ucc.asn.au/dropbear/releases/$DROPBEAR.tar.bz2
  [$OPENSSH]=https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/$OPENSSH.tar.gz
  [$BASH_V]=https://ftp.gnu.org/gnu/bash/$BASH_V.tar.gz
  [$TOYBOX]=https://landley.net/toybox/downloads/$TOYBOX.tar.gz)
declare -A SHA=(
  [$DROPBEAR]=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
  [$OPENSSH]=a9dc9565dffe8640f64d863cd29a32bc4a3dbdec0566a7fc44c5d6ee767d5f39
  [$BASH_V]=9599b22ecd1d5787ad7d3b7bf0c59f312b3396d1e281175dd1f8a4014da621ff
  [$TOYBOX]=44e2bab43dfe2c171743297631ce558c6d9ed38b87a66fd751cef617574b375e)
# toybox: commands added to the macOS set, and ones that do not build against the iPhoneOS SDK (awk: system() is unavailable on
# iOS; stty: linux/tty.h)
TOYBOX_ADD="VI DIFF EXPR TR MORE HEXDUMP XZCAT"
TOYBOX_DROP="READAHEAD"

export GPU_LOCAL_SDK=$C/apple-sdk/iPhoneOS.sdk GPU_LOCAL_CLANG_RT=$C/apple-sdk/toolchain/clang-rt/libclang_rt.ios.a
export GPU_LOCAL_RCODESIGN=$(ls "$C"/toolchain/apple-codesign-*/rcodesign 2>/dev/null | sort -V | tail -1)
[ -d "$GPU_LOCAL_SDK" ] || { echo "no SDK at $GPU_LOCAL_SDK (scripts/build-gpu-plugin-local.sh --fetch-sdk)"; exit 1; }
[ -x "$GPU_LOCAL_RCODESIGN" ] || { echo "no rcodesign under $C/toolchain (scripts/build-gpu-plugin-local.sh installs it)"; exit 1; }
export SDKROOT=$GPU_LOCAL_SDK
# The cross tools by full path, not on PATH: the build-host compilers (bash's mkbuiltins, toybox's kconfig) must find the host ld.
GL=$BOARD/scripts/gpu-local/bin
TGT="-target arm64-apple-ios16.0 -isysroot $GPU_LOCAL_SDK"
XCC="$GL/clang $TGT"
HOSTCC="gcc -std=gnu17"
J=$(nproc)
say() { echo "[$(date +%T)] $*"; }

mkdir -p "$SRC" "$B"
for n in "$DROPBEAR" "$OPENSSH" "$BASH_V" "$TOYBOX"; do
  f=$SRC/$(basename "${URL[$n]}")
  [ -s "$f" ] || { say "fetching $n"; curl -sfL -o "$f.part" "${URL[$n]}" && mv "$f.part" "$f"; }
  echo "${SHA[$n]}  $f" | sha256sum -c --quiet - || { echo "$f: sha256 mismatch"; exit 1; }
  if [ ! -d "$B/$n" ]; then
    tar xf "$f" -C "$B"
    case $n in
      "$DROPBEAR") GIT_CEILING_DIRECTORIES=$B git -C "$B/$n" apply -p1 "$P/dropbear-fajita.patch" ;;
      "$BASH_V") GIT_CEILING_DIRECTORIES=$B git -C "$B/$n" apply -p1 "$P/bash-fajita.patch" ;;
    esac
  fi
done

say "dropbear"
cd "$B/$DROPBEAR"
cp "$P/dropbear-localoptions.h" localoptions.h
[ -f Makefile ] || ./configure --host=aarch64-apple-darwin CC="$XCC" AR="$GL/ar" RANLIB="$GL/ranlib" CFLAGS=-O2 --disable-zlib \
  --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-lastlog --disable-pam --enable-bundled-libtom > configure.out 2>&1
make -j"$J" PROGRAMS="dropbear dropbearkey scp dbclient" > make.out 2>&1 || { tail -20 make.out; exit 1; }

say "sftp-server (OpenSSH)"
cd "$B/$OPENSSH"
# getentropy is in libSystem but the SDK has no <sys/random.h>: use the /dev/urandom path
[ -f Makefile ] || ./configure --host=aarch64-apple-darwin CC="$XCC" AR="$GL/ar" RANLIB="$GL/ranlib" CFLAGS=-O2 ac_cv_func_getentropy=no \
  --without-openssl --without-zlib --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx --disable-lastlog --without-pam \
  --disable-security-key > configure.out 2>&1
make -j"$J" sftp-server > make.out 2>&1 || { tail -20 make.out; exit 1; }

say "bash"
cd "$B/$BASH_V"
[ -f Makefile ] || ./configure --host=aarch64-apple-darwin CC="$XCC" AR="$GL/ar" RANLIB="$GL/ranlib" CC_FOR_BUILD="$HOSTCC" \
  CFLAGS_FOR_BUILD=-O1 CFLAGS="-O2 -std=gnu17 -Wno-implicit-function-declaration -Wno-incompatible-function-pointer-types" \
  --without-bash-malloc --disable-nls --enable-readline --with-curses > configure.out 2>&1
make -j"$J" > make.out 2>&1 || { tail -20 make.out; exit 1; }

say "toybox"
cd "$B/$TOYBOX"
{ grep -v -E "^CONFIG_(${TOYBOX_DROP// /|})=" scripts/macos_miniconfig; for a in $TOYBOX_ADD; do echo "CONFIG_$a=y"; done; } > ios_miniconfig
export LDOPTIMIZE=-Wl,-dead_strip   # toybox picks the linker flags by the build host's uname
KCONFIG_ALLCONFIG=ios_miniconfig HOSTCC="$HOSTCC" scripts/genconfig.sh -n > genconfig.out 2>&1
# toybox's library probe pipes a test program through stdin, which the stand-in clang does not take: name the libraries
make toybox HOSTCC="$HOSTCC" CC="$GL/clang" CFLAGS="$TGT -O2 -I$P/ios-compat" LDFLAGS="$TGT -liconv -lutil -lresolv -lz" \
  STRIP="$GL/strip" > make.out 2>&1 || { grep -E 'error' make.out | head; exit 1; }

say "staging $OUT"
T=$(mktemp -d "$C/ssh-stage.XXXX"); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/libexec" "$T/etc" "$T/share/terminfo"
cp "$B/$DROPBEAR"/{dropbear,dropbearkey,scp,dbclient} "$T/bin/"
cp "$B/$OPENSSH/sftp-server" "$T/libexec/"
cp "$B/$BASH_V/bash" "$T/bin/"
cp "$B/$TOYBOX/toybox" "$T/bin/"; chmod u+w "$T/bin/toybox"
cp "$P/ssh-start" "$T/libexec/ssh-start"
cp "$P/profile" "$T/etc/profile"; cp "$P/bashrc" "$T/etc/bashrc"
printf 'UniXNU fajita (iOS 16.7.16) SSH dev mode. The add-on is /private/var/ngpu/ssh; state, keys and logs are in /private/var/ssh.\n' > "$T/etc/motd"
# Public keys only. A builder without keys gets an add-on nobody can log in to.
KEYS=${FAJITA_SSH_KEYS:-$(ls "$HOME"/.ssh/*.pub 2>/dev/null | tr '\n' ' ')}
: > "$T/etc/authorized_keys"
for k in $KEYS; do grep -E '^(ssh-|ecdsa-|sk-)' "$k" >> "$T/etc/authorized_keys" || true; done
grep -q 'PRIVATE KEY' "$T/etc/authorized_keys" && { echo "refusing: a private key in FAJITA_SSH_KEYS"; exit 1; }
say "authorized keys: $(wc -l < "$T/etc/authorized_keys") (from ${KEYS:-nothing})"
# terminfo: iOS has libncurses 5.4 but no database. ncurses 5 reads only the legacy 16-bit format, so entries whose numbers do
# not fit (pairs#0x10000) are clamped. Both the letter and the hex directory layouts are written.
for t in xterm-256color xterm xterm-color screen-256color screen tmux-256color tmux vt100 vt220 linux dumb ansi; do
  infocmp -x "$t" > "$T/ti.src" 2>/dev/null || continue
  sed -i -E 's/pairs#(0x10000|65536|[0-9]{6,})/pairs#32767/' "$T/ti.src"
  tic -o "$T/share/terminfo" "$T/ti.src" 2>/dev/null || true
done
rm -f "$T/ti.src"
for d in "$T"/share/terminfo/?; do [ -d "$d" ] || continue; h=$(printf '%x' "'$(basename "$d")"); mkdir -p "$T/share/terminfo/$h"; cp "$d"/* "$T/share/terminfo/$h/"; done
for f in $(od -An -tx1 -N2 "$T"/share/terminfo/x/xterm-256color | tr -d ' '); do [ "$f" = 1a01 ] || { echo "terminfo xterm-256color is not in the legacy format"; exit 1; }; done
chmod 755 "$T/libexec/ssh-start"
for f in "$T"/bin/* "$T"/libexec/sftp-server; do
  "$GL/strip" -x "$f" 2>/dev/null || true
  "$GL/codesign" --force --sign - --pagesize 4096 --identifier "org.blu-sharky.ssh.$(basename "$f")" \
    --entitlements "$BOARD/package/usb-debug-tools/fjdbg-agent-entitlements.plist" "$f" > /dev/null
  "$GL/codesign" --verify --strict "$f"
done
rm -rf "$OUT.new"; cp -r "$T" "$OUT.new"; rm -rf "$OUT"; mv "$OUT.new" "$OUT"
say "built $OUT: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"
