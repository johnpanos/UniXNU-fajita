#!/bin/bash
# proc-sample.sh NAME|PID [SECONDS] [HZ] [all]: where a process's threads spend their time, on the held session: fjsurface --sample (task_for_pid;
# each thread suspended, its pc, lr and frame-pointer chain read, resumed; default 5 s at 100 Hz), then every address symbolized on the host
# against the shared cache (tools/crash/dsc-sym.py with the process's slide). Prints the running threads and their most frequent stacks,
# top frame first; "all" also the waiting threads (a client blocked in an XPC call). For a daemon that spins (coreduetd, healthappd, mobileassetd in the gfx suite's scenes): what it is doing.
# Frames of the GPU plugin (NativeAdrenoMetal+OFF) are named from its unstripped copy in SYMS (default ~/src/UniXNU-fajita-stack/build/
# adreno-metal-plugin: it must be the build the phone runs). Output also in $FAJITA_WORK/proc-sample/NAME.txt. fjsurface comes from $FAJITA_OUT/tools (scripts/fajita/build-usbdbg-agent.sh).
TD=$(cd "$(dirname "$0")" && pwd); BOARD=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$BOARD/.cache/fajita-work}; OUTD=${FAJITA_OUT:-$BOARD/.cache/fajita-out}; SS=$TD/session.sh
[ -n "$1" ] || { sed -n 2,6p "$0"; exit 2; }
f=$OUTD/tools/fjsurface; [ -f "$f" ] || { echo "no $f (scripts/fajita/build-usbdbg-agent.sh $OUTD/tools/fjdbg-agent)"; exit 1; }
p=/private/var/ngpu/sbin/fjsurface-$(sha1sum "$f" | cut -c1-8)
T=120 $SS push "$f" $p >/dev/null 2>&1
pid=$1
if ! [[ $pid =~ ^[0-9]+$ ]]; then pid=$(T=15 $SS run /usr/sbin/ngpu-metal ps 2>/dev/null | awk -v n="$1" '$2==n{print $1; exit}'); fi
[ -n "$pid" ] || { echo "proc-sample: no process $1"; exit 1; }
mkdir -p $W/proc-sample; out=$W/proc-sample/$1.txt
secs=${2:-5}
T=$(( ${secs%.*} + 30 )) $SS run $p --sample $pid $secs ${3:-100} ${4:-} > $out.raw 2>&1
python3 -I - "$out.raw" "$BOARD" > $out <<'EOF'
import re, subprocess, sys
raw, board = sys.argv[1], sys.argv[2]
lines = open(raw, errors='replace').read().splitlines()
slide = next((m.group(1) for l in lines for m in [re.search(r'slide (0x[0-9a-f]+)', l)] if m), '0x0')
mainbase = int(next((m.group(1) for l in lines for m in [re.search(r'main (0x[0-9a-f]+)', l)] if m), '0x0'), 16)
frames = sorted({a for l in lines if l.startswith('  ') and ':' in l for a in l.split(':', 1)[1].split()})
addrs = [a for a in frames if '+' not in a]
names = {}
# Frames of images outside the shared cache (NAME+OFFSET): the GPU plugin's from its unstripped copy (SYMS, default the stack's build)
import os
syms = os.environ.get('SYMS') or os.path.expanduser('~/src/UniXNU-fajita-stack/build/adreno-metal-plugin')
for a in frames:
    if '+' not in a: continue
    image, off = a.split('+', 1)
    obj = os.path.join(syms, image + '.unstripped')
    if os.path.exists(obj):
        r = subprocess.run(['llvm-symbolizer', '--obj=' + obj, '--functions=linkage', '--no-inlines', '0x' + off], capture_output=True, text=True)
        fn = r.stdout.split('\n')[0].strip()
        d = subprocess.run(['llvm-cxxfilt', fn], capture_output=True, text=True).stdout.strip() if fn else ''
        names[a] = (image + ' ' + (d or fn))[:110]
if addrs:
    r = subprocess.run(['python3', board + '/tools/crash/dsc-sym.py', '--demangle'] + addrs, capture_output=True, text=True, timeout=300)
    for l in r.stdout.splitlines():
        a, _, rest = l.partition(':')
        rest = rest.strip()
        m = re.match(r'0x[0-9a-f]+ (.*)', rest)
        names[a.strip()] = (m.group(1) if m else rest)[:110]
for l in lines:
    if l.startswith('  ') and ':' in l:
        n, frames = l.split(':', 1)
        print('  %s samples:' % n.strip())
        for a in frames.split():
            n = names.get(a, '?')
            if mainbase and 'not in any image' in n:   # the process's own executable: offset from its load address (for dsc-disasm/otool)
                n = 'main +%#x' % (int(a, 16) + int(slide, 16) - mainbase)
            print('      %-12s %s' % (a, n))
    else:
        print(l)
EOF
cat $out
