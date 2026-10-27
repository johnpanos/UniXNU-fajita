#!/bin/bash
# gfx-boots.sh IMAGE_DIR [N [SCENE...]]: N fresh boots (default 4) of a built image (FAJITA_OUT-style directory: payload.img, boot.img,
# its kernel), each running SCENEs (default home) once right after the boot, with backboardd left as it booted (no restart, no warm-up,
# no frame trace) and the occurrence catcher on: how often a glitch that depends on the boot shows up, and what the catcher caught.
# Runs are $FAJITA_WORK/gfx/TAG-I (TAG, default boots). For intermittent correctness bugs (the m2v dock-icon smear, NOTES-gfx.md).
#   COLD=1: before each boot after the first, every process's pipeline cache is made cold (gpu-env NGPU_M2V_DIR set to a new name);
#   the plain gpu-env is restored at the end, also when a boot fails.
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}; SS=$TD/session.sh
O=$(realpath "${1:?image directory}"); N=${2:-4}; shift 2 2>/dev/null; SCENES=${*:-home}; tag=${TAG:-boots}
[ -f "$O/payload.img" ] || { echo "gfx-boots: no payload in $O"; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf $tmp' EXIT
restore() {   # a session to put the plain gpu-env back (COLD=1)
  local out s; out=$(FAJITA_OUT=$O $SS start 2>&1); s=$(grep -o 'session-[0-9]\{6\}' <<<"$out" | tail -1)
  printf '# plain\n' > $tmp/plain; T=60 $SS push $tmp/plain /private/var/ngpu/gpu-env 644 >/dev/null 2>&1; T=60 $SS end $s >/dev/null 2>&1
}
cold_pending=0
for i in $(seq 1 $N); do
  out=$(FAJITA_OUT=$O $SS start 2>&1); sess=$(grep -o 'session-[0-9]\{6\}' <<<"$out" | tail -1)
  if ! grep -q 'ready after' <<<"$out"; then
    echo "$i $sess: boot failed: $(tail -1 <<<"$out")"; T=60 $SS end $sess >/dev/null 2>&1; continue
  fi
  KM=$O/unixnu/fajita-ios16/kernel/kernel.macho TRACE=0 NORESTART=1 WARMUP=0 DIAG=1 PLAN_S=60 \
    $TD/gfx-suite.sh $tag-$i $SCENES > $W/gfx/$tag-$i.txt 2>&1
  echo "$i $sess: $(grep -h 'shots:' $W/gfx/$tag-$i/report.txt | tr '\n' ' ') $(grep -h 'CATCH' $W/gfx/$tag-$i.txt | tr '\n' ' ')"
  if [ "${COLD:-0}" = 1 ] && [ $i -lt $N ]; then
    printf 'NGPU_M2V_DIR=cold%s\n' "$(date +%H%M%S)" > $tmp/env; T=60 $SS push $tmp/env /private/var/ngpu/gpu-env 644 >/dev/null 2>&1; cold_pending=1
  fi
  [ "${COLD:-0}" = 1 ] && [ $i -eq $N ] && { printf '# plain\n' > $tmp/plain; T=60 $SS push $tmp/plain /private/var/ngpu/gpu-env 644 >/dev/null 2>&1; cold_pending=0; }
  T=60 $SS end $sess >/dev/null 2>&1
done
[ $cold_pending = 1 ] && restore
exit 0
