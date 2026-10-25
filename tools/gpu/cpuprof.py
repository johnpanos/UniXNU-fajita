#!/usr/bin/env python3
"""cpuprof.py CPUPROF.txt [--stack DIR] [--thread tN] [--only enc|cpu|all] [--rows N]: the NativeMetal CPU profile
(NGPU_CPU_PROFILE, CpuProfile.h; scripts/fajita/test/passtrace.sh pulls it with CPU=1) -> where the render server's command-buffer
threads spend their time.

The phone writes each distinct stack once with four counts: enc (running while a command buffer is open: the frame trace's encode window),
cpu (running, none open), blocked_enc / blocked_other (in the kernel). Frames of the plugin's own images (NativeAdrenoMetal,
libvulkan_freedreno) are image+0xOFFSET; they are resolved here with llvm-nm and llvm-cxxfilt against the unstripped copies the plugin build keeps
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
ap.add_argument('--under', help='a function (substring): the split of its samples by the callee directly under it (self = in it)')
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
    # the image's symbol table (llvm-nm, address order), demangled (llvm-cxxfilt: Itanium and Rust v0); each offset -> the symbol at or
    # below it (a return address points after its call: look up offset - 4)
    nm = subprocess.run(['llvm-nm', '-n', '--defined-only', obj], capture_output=True, text=True).stdout.splitlines()
    syms = [(int(f[0], 16), f[2][1:] if f[2].startswith('_') else f[2]) for f in (l.split(None, 2) for l in nm) if len(f) == 3 and f[1] in 'tTsS']
    dem = subprocess.run(['llvm-cxxfilt'], input='\n'.join(n for _, n in syms), capture_output=True, text=True).stdout.splitlines()
    if len(dem) == len(syms):
        syms = [(ad, re.sub(r'::h[0-9a-f]{16}$', '', d)) for (ad, _), d in zip(syms, dem)]
    addrs = [ad for ad, _ in syms]
    import bisect
    for o in offs:
        i = bisect.bisect_right(addrs, o - 4 if o >= 4 else o) - 1
        fn = syms[i][1] if i >= 0 else '0x%x' % o
        names['%s+0x%x' % (img, o)] = '%s`%s' % (img.split('.')[0], fn)

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
if a.under:
    kids, n_under = collections.Counter(), 0
    for e, c, be, bo, st in rows:
        if a.thread and st[0] != a.thread:
            continue
        n = pick(e, c)
        frames = [name(f) for f in st[1:]]
        idx = [i for i, f in enumerate(frames) if a.under in f]
        if not n or not idx:
            continue
        i = idx[-1]   # the outermost occurrence
        j = i - 1
        while j >= 0 and frames[j] == frames[i]:
            j -= 1    # the walk repeats a frame when lr and the first saved return agree
        n_under += n
        kids['(self)' if j < 0 else frames[j]] += n
    print('\nunder %s (%s): %d samples' % (a.under, a.only, n_under))
    for f, n in kids.most_common(a.rows):
        print('%7d %5.1f%%  %s' % (n, 100 * n / n_under, f[:170]))
    sys.exit(0)
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
