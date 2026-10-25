#!/bin/bash
# passtrace.sh: per-pass GPU time of the render server, aggregated on the phone (NativeMetal PassTrace.h), in a running session.
#   passtrace.sh on [EXTRA_ENV_FILE]   gpu-env NGPU_FRAME_TRACE=1 + NGPU_PASS_TRACE=backboardd (+ the lines of EXTRA_ENV_FILE), restart backboardd
#   passtrace.sh reset                 clear the phone's aggregate (it covers what follows: run it right before a measurement)
#   passtrace.sh pull [OUT]            the summary (default $FAJITA_WORK/passes.csv), then tools/gpu/frametrace.py --passes on it
#   passtrace.sh off                   the plain gpu-env back (empty), restart backboardd
#   passtrace.sh fps [OUT]             reset, fps.sh, pull: the per-pass split of the standard benchmark
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd); W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}; mkdir -p "$W"; H=$TD/hc
DIR=/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/frametrace
restart() {
  T=30 $H run /usr/sbin/ngpu-metal killall backboardd >/dev/null
  for i in $(seq 1 20); do sleep 3; T=15 $H run /usr/sbin/ngpu-metal ps 2>/dev/null | grep -q backboardd && break; done
  sleep ${SETTLE_S:-20}; T=8 $H kcmd touch swipe up >/dev/null; sleep 2
}
pushenv() {
  T=60 $H push "$1" /private/var/ngpu/gpu-env 644 >/dev/null 2>&1
  T=60 $H pull /private/var/ngpu/gpu-env "$W/.pt-env-check" >/dev/null 2>&1; cmp -s "$1" "$W/.pt-env-check" || { echo "passtrace: gpu-env push failed"; exit 1; }
}
reset() { printf 'reset\n' > "$W/.pt-reset"; T=30 $H push "$W/.pt-reset" $DIR/passtrace-reset 644 >/dev/null 2>&1; sleep 2; }
pull() { out=${1:-$W/passes.csv}; T=60 $H pull $DIR/backboardd-passes.csv "$out" >/dev/null 2>&1 || { echo "passtrace: pull failed"; exit 1; }
  python3 -I "$ROOT/tools/gpu/frametrace.py" --passes "$out"; }
case "$1" in
  on) { printf 'NGPU_FRAME_TRACE=1\nNGPU_PASS_TRACE=backboardd\n'; [ -n "$2" ] && cat "$2"; } > "$W/.pt-env"; pushenv "$W/.pt-env"; restart ;;
  off) printf '# plain\n' > "$W/.pt-env"; pushenv "$W/.pt-env"; restart ;;
  reset) reset ;;
  pull) pull "$2" ;;
  fps) reset; "$TD/fps.sh"; pull "$2" ;;
  *) sed -n '2,7p' "$0"; exit 2 ;;
esac
