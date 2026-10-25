#!/bin/bash
# leakwatch.sh [RUNS]: run fps.sh RUNS times (default 4) back to back in the current session and print, per run, the frame rates and what
# each submission cost and carried in the kernel (kcmd gpu: retire and total us per submit, owned BO bytes and objects published per
# submit) plus the frontend's live-object counters (rows.py: no-copy buffers, surface imports, XR textures/variants). A number that grows
# run over run while the frame rate falls is the suspect. Output also appended to $FAJITA_WORK/leakwatch.txt.
TD=$(cd "$(dirname "$0")" && pwd); W=${FAJITA_WORK:-$(cd "$TD/../../.." && pwd)/.cache/fajita-work}; mkdir -p "$W"; H=$TD/hc
OUT=$W/leakwatch.txt; RUNS=${1:-4}
snap() {   # n retire_us total_us own_bytes objects(x100)
  local raw g; raw=$($H kcmd gpu raw 999999999 | grep '^now'); g=$($H kcmd gpu)
  local n=$(echo "$raw" | awk '{print $4}')
  local own=$(echo "$raw" | awk -F'|' '{print $2}' | awk '{print $2}')
  local avgs=$(echo "$g" | grep -o 'retire [0-9]* .*total [0-9]*' | awk '{print $2, $12}')
  local obj=$(echo "$g" | grep -o 'owned [0-9]* KiB in [0-9]*' | awk '{print $5}')
  echo "$n $avgs $own $obj"
}
row() { python3 -I $TD/rows.py - 2>/dev/null | grep -E "^N no-copy|^N xr|^S |^N surface gen" | tr '\n' ' ' | cut -c1-260; }
echo "== $(date +%T) leakwatch $RUNS runs" | tee -a $OUT
for r in $(seq 1 $RUNS); do
  read n0 r0 t0 o0 b0 <<< "$(snap)"
  fps=$($TD/fps.sh 2>&1 | tail -1)
  read n1 r1 t1 o1 b1 <<< "$(snap)"
  python3 -I -c "
n0,r0,t0,o0,b0,n1,r1,t1,o1,b1=[float(x or 0) for x in '$n0 $r0 $t0 $o0 $b0 $n1 $r1 $t1 $o1 $b1'.split()]
d=max(n1-n0,1)
print('run $r: %d submits | retire %.1f ms/submit total %.1f ms/submit | owned %.0f MiB/submit, %.0f objects/submit' %
      (n1-n0,(r1*n1-r0*n0)/d/1000,(t1*n1-t0*n0)/d/1000,(o1-o0)/d/1048576,(b1*n1-b0*n0)/d/100))" | tee -a $OUT
  echo "  $fps" | tee -a $OUT
  echo "  $(row)" | tee -a $OUT
done
