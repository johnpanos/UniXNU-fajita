#!/usr/bin/env python3
"""cpuprof.py CPUPROF.txt [--stack DIR] [--thread tN] [--only enc|cpu|all] [--rows N]: the NativeMetal CPU profile
(NGPU_CPU_PROFILE, CpuProfile.h; scripts/fajita/test/passtrace.sh pulls it with CPU=1) -> where the render server's command-buffer
threads spend their time.

The phone writes each distinct stack once with four counts: enc (running while a command buffer is open: the frame trace's encode window),
cpu (running, none open), blocked_enc / blocked_other (in the kernel). Frames of the plugin's own images (NativeAdrenoMetal,
libvulkan_freedreno) are image+0xOFFSET; they are resolved here with llvm-symbolizer against the unstripped copies the plugin build keeps
(<stack>/build/adreno-metal-plugin/*.unstripped; --stack, default ~/src/UniXNU-fajita-stack). They match only the build that ran.
Apple's libraries read <redacted> (the shared cache's local symbols).

Prints per thread the sample split, then (for --only, default enc) the top functions by self samples, by inclusive samples, the
functions on the stack while blocked inside encode, and the top stacks."""
import argparse, collections, os, re, subprocess, sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('profile')
ap.add_argument('--stack', default=os.path.expanduser('~/src/UniXNU-fajita-stack'))
ap.add_argument('--thread')
ap.add_argument('--only', choices=('enc', 'cpu', 'all'), default='enc')
ap.add_argument('--rows', type=int, default=30)
a = ap.parse_args()

head, rows = '', []
for line in open(a.profile, errors='replace'):
    if line.startswith('#'):
        head = head or line.strip()
        continue
    f = line.rstrip('\n').split(',', 4)
    if len(f) == 5:
        rows.append((*map(int, f[:4]), f[4].split(';')))
print(head)

images = {'NativeAdrenoMetal': 'NativeAdrenoMetal.unstripped', 'libvulkan_freedreno.dylib': 'libvulkan_freedreno.unstripped'}
want = collections.defaultdict(set)
for *_, st in rows:
    for fr in st:
        m = re.match(r'(.+)\+0x([0-9a-f]+)$', fr)
        if m:
            want[m.group(1)].add(int(m.group(2), 16))
names = {}
for img, offs in want.items():
    obj = os.path.join(a.stack, 'build/adreno-metal-plugin', images.get(img, img))
    if not os.path.exists(obj):
        print('cpuprof: no %s: %s frames stay offsets' % (obj, img), file=sys.stderr)
        continue
    offs = sorted(offs)
    # a return address points after the call: look up the call itself (-4)
    inp = '\n'.join('0x%x' % (o - 4 if o >= 4 else o) for o in offs)
    out = subprocess.run(['llvm-symbolizer', '--obj', obj, '--demangle', '--no-inlines', '--functions=short', '--output-style=GNU'],
                         input=inp, capture_output=True, text=True).stdout
    lines = out.splitlines()   # two lines per address: the containing function, file:line
    i = 0
    for o in offs:
        if i >= len(lines):
            break
        fn = lines[i].strip()
        i += 2
        fn = re.sub(r'::h[0-9a-f]{16}$', '', fn)
        fn = re.sub(r'<[^<>]*>', '<>', fn) if len(fn) > 140 else fn
        names['%s+0x%x' % (img, o)] = '%s`%s' % (img.split('.')[0], fn if fn != '??' else '0x%x' % o)

name = lambda fr: names.get(fr, fr)
threads = collections.defaultdict(lambda: [0, 0, 0, 0])
for e, c, be, bo, st in rows:
    t = threads[st[0]]
    t[0] += e; t[1] += c; t[2] += be; t[3] += bo
print('%-6s %8s %8s %11s %13s' % ('thread', 'enc', 'cpu', 'blocked_enc', 'blocked_other'))
for t, v in sorted(threads.items(), key=lambda kv: -sum(kv[1])):
    print('%-6s %8d %8d %11d %13d' % (t, *v))

pick = {'enc': lambda e, c: e, 'cpu': lambda e, c: c, 'all': lambda e, c: e + c}[a.only]
self_, incl, blocked, stacks = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
for e, c, be, bo, st in rows:
    if a.thread and st[0] != a.thread:
        continue
    frames = [name(f) for f in st[1:]]
    n = pick(e, c)
    nb = pick(be, bo)
    if n:
        self_[frames[0]] += n
        for f in dict.fromkeys(frames):
            incl[f] += n
        stacks[' < '.join(frames[:7])] += n
    if nb:
        for f in dict.fromkeys(frames):
            blocked[f] += nb
total = sum(self_.values())
print('\n%s running samples (1 ms each): %d' % (a.only, total))
for title, cnt in (('self', self_), ('inclusive', incl)):
    print('\n%s:' % title)
    for f, n in cnt.most_common(a.rows):
        print('%7d %5.1f%%  %s' % (n, 100 * n / total if total else 0, f[:160]))
print('\nblocked (%s), functions on the stack:' % a.only)
for f, n in blocked.most_common(a.rows):
    print('%7d  %s' % (n, f[:160]))
print('\ntop stacks (%s, running):' % a.only)
for s, n in stacks.most_common(min(a.rows, 20)):
    print('%7d  %s' % (n, s[:400]))
