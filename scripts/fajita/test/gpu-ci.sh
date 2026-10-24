#!/bin/bash
# gpu-ci.sh RUN_ID [MAX_S]: follow a CI run with a line per state change (job/step), never silent for more than 60 s.
# Ends at completion: success -> exit 0; failure -> the failing step's last 40 log lines, exit 1; MAX_S (default 1500) -> exit 2.
# Run it in the background and read its output with short checks; don't block on it.
R=$1; MAX=${2:-1500}; t0=$(date +%s); last=""; beat=$t0
while :; do
  j=$(gh run view $R -R johnpanos/UniXNU-fajita --json status,conclusion,jobs 2>/dev/null) || { echo "$(date +%T) gh error"; sleep 15; continue; }
  s=$(echo "$j" | python3 -I -c '
import json,sys; d=json.load(sys.stdin); cur=[]
for jb in d["jobs"]:
    st=[s for s in jb.get("steps",[]) if s["status"]=="in_progress"]
    cur.append("%s: %s" % (jb["name"], st[0]["name"] if st else jb["status"]+"/"+(jb.get("conclusion") or "")))
print(d["status"], d.get("conclusion") or "-", "|", "; ".join(cur))')
  now=$(date +%s)
  if [ "$s" != "$last" ] || [ $((now - beat)) -ge 60 ]; then echo "$(date +%T) +$((now - t0))s $s"; last=$s; beat=$now; fi
  case $s in
    completed\ success*) exit 0;;
    completed*) echo "--- failed log tail ---"; gh run view $R -R johnpanos/UniXNU-fajita --log-failed 2>/dev/null | tail -40; exit 1;;
  esac
  [ $((now - t0)) -ge $MAX ] && { echo "gave up after ${MAX}s"; exit 2; }
  sleep 15
done
