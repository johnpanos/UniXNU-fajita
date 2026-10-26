#!/bin/bash
# ca-capture.sh [FRAMES [SKIP]]: capture FRAMES (default 120) of the render server's command buffers in motion, after SKIP (default
# 600: past the lock screen and the first home frames), as metal2vulkan command-replay records (the GPU plugin's frame capture:
# NGPU_CAPTURE + NGPU_CAPTURE_FRAMES, drivers/NativeMetal/PipelineCapture.h), and pull them over Wi-Fi SSH (the capture is tens of MB,
# too big for the USB hub). For metal2vulkan's host replay (command-replay, M2V_REPLAY_TIMES: the encode time per draw on CA's real
# frames). On the held session; needs the SSH dev mode (boot with WIFI_EXTRA_ARGS fajita-ssh=1, scripts/fajita/devmode-ssh.sh find).
#   Steps: gpu-env with the capture keys -> restart backboardd -> SETTLE_S (25) -> unlock swipe, then home page swipes (left/right)
#   until capture.done (MAX_S, 180) -> pull /private/var/tmp/ngpu-cap/backboardd-PID to $FAJITA_WORK/ca-capture/backboardd-PID.
#   On exit, always: the plain gpu-env back and backboardd restarted, so no later run measures with the capture on (AGENTS.md).
#   Env: M2V (a metal2vulkan checkout, default ~/src/metal2vulkan-gfx) for the manifest (scripts/pipeline-census/frames_manifest.py).
# (metal2vulkan's scripts/pipeline-census/fajita-pull pulls the same capture over the USB hub: fine for the pipeline census, slow here.)
# The capture holds Apple's shaders and UI pixels: it stays under .cache (FAJITA_WORK), never in git.
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}; SS=$TD/session.sh
N=${1:-120}; K=${2:-600}; G=$W/ca-capture; mkdir -p $G
DIR=/private/var/tmp/ngpu-cap
restore() {
  printf '# plain\n' > $G/.plain
  T=60 $SS push $G/.plain /private/var/ngpu/gpu-env 644 >/dev/null 2>&1
  T=30 $SS run /usr/sbin/ngpu-metal killall backboardd >/dev/null 2>&1
  echo "ca-capture: plain gpu-env restored, backboardd restarted"
}
trap restore EXIT
addr=$($BOARD/scripts/fajita/devmode-ssh.sh find 2>/dev/null | head -1)
[ -n "$addr" ] || { echo "ca-capture: the phone's SSH dev mode is not reachable (boot with fajita-ssh=1)"; trap - EXIT; exit 1; }
printf 'NGPU_CAPTURE=%s\nNGPU_CAPTURE_FRAMES=%s\nNGPU_CAPTURE_SKIP=%s\nNGPU_CAPTURE_MAX=268435456\n' $DIR $N $K > $G/gpu-env
T=60 $SS push $G/gpu-env /private/var/ngpu/gpu-env 644 >/dev/null 2>&1
T=60 $SS pull /private/var/ngpu/gpu-env $G/gpu-env.check >/dev/null 2>&1
cmp -s $G/gpu-env $G/gpu-env.check || { echo "ca-capture: gpu-env push failed"; exit 1; }
T=30 $SS run /usr/sbin/ngpu-metal killall backboardd >/dev/null
sleep ${SETTLE_S:-25}
pid=$(T=15 $SS run /usr/sbin/ngpu-metal ps 2>/dev/null | awk '$2=="backboardd"{print $1; exit}')
[ -n "$pid" ] || { echo "ca-capture: no backboardd after the restart"; exit 1; }
echo "ca-capture: backboardd $pid, capturing $N command buffers after $K"
T=10 $SS call kcmd touch swipe up >/dev/null 2>&1; sleep 2
end=$(( $(date +%s) + ${MAX_S:-180} )); done=0
while [ $(date +%s) -lt $end ]; do
  for d in left right; do T=10 $SS call kcmd touch swipe $d >/dev/null 2>&1; sleep 1; done
  if T=20 $SS pull $DIR/backboardd-$pid/capture.done $G/.done >/dev/null 2>&1; then done=1; break; fi
done
[ $done = 1 ] || { echo "ca-capture: no capture.done after ${MAX_S:-180} s"; exit 1; }
port=$($BOARD/scripts/fajita/devmode-ssh.sh find 2>/dev/null | sed -n 's/.*scp .* -P \([0-9]*\) .*/\1/p')
rm -rf $G/backboardd-$pid
scp -q -r -o HostKeyAlias=fajita -o ConnectTimeout=10 -P ${port:-22} root@$addr:$DIR/backboardd-$pid $G/ \
  || { echo "ca-capture: scp failed"; exit 1; }
T=60 $BOARD/scripts/fajita/devmode-ssh.sh ssh "rm -rf $DIR/backboardd-$pid" >/dev/null 2>&1
echo "ca-capture: $(du -sh $G/backboardd-$pid | cut -f1) in $G/backboardd-$pid"
M2V=${M2V:-$HOME/src/metal2vulkan-gfx}
if [ -f $M2V/scripts/pipeline-census/frames_manifest.py ]; then
  python3 -I $M2V/scripts/pipeline-census/frames_manifest.py $G/backboardd-$pid --out $G/backboardd-$pid/frames.json \
    && echo "ca-capture: manifest $G/backboardd-$pid/frames.json; replay: M2V_REPLAY_TIMES=20 command-replay run --manifest ... (metal2vulkan)"
fi
