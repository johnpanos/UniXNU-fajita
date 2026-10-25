#!/usr/bin/env python3
"""frametrace.py TRACE.csv [--t0 US --t1 US] [--worst PCT] [--rows N] [--json OUT]: the NativeMetal frame trace (NGPU_FRAME_TRACE=1,
FrameTrace.h in NativeMetal) -> per-frame timing, the worst frames and where their time went.
frametrace.py --passes PASSES.csv: the per-pass GPU split of the pass trace (NGPU_PASS_TRACE=backboardd, PassTrace.h in NativeMetal;
scripts/fajita/test/passtrace.sh pulls it): per pass key the count per presented frame, ms per frame, median/p95/max ms, draws.

A frame is the interval between two scanout swap offers (S lines; gaps over --idle ms, default 250, are idle, not frames). Each frame
owns the render server's command buffers (C lines, every queue but the presenter's "ngpu.present") committed in it, the presenter's
copy of its swap, the waits (W) and pipeline/library/function creations (P) that started in it. Components per frame (ms):
  cpu_gap    from the previous offer to the first command buffer made (the render server's own CPU work: layout, commit, vsync wait)
  encode     created -> commit of its command buffers (Metal encoding on the CPU, incl. any pipeline creation and waits inside)
  pipe       pipeline / library / function creation while they were encoded (part of encode)
  wait       waits for earlier GPU work while they were encoded (part of encode)
  flush      cache maintenance / host copies (NGPU_HOST_COPY) of imports and surfaces at commit (part of commit)
  commit     commit -> commit returned (flush, translation, Vulkan submission)
  gpu        the GPU time of its command buffers (translator timestamps, gend - gstart)
  gpu_queue  the last command buffer's commit return -> its GPU start (behind earlier GPU work, e.g. the presenter's copy)
  complete   GPU end -> completion seen by the frontend
  to_swap    the last command buffer's handlers -> the swap offer (the render server's swap path, the kernel's hand-off)
  present    swap offered -> its GPU copy returned (the presenter); present_gpu its copy's GPU time
Prints the frame-time distribution, the worst frames (top PCT %, default 7, plus every frame over 33.3 ms) with their components,
and the ranked causes: each component's mean on the worst frames against the median frames.
Times in the trace are us of CLOCK_UPTIME_RAW (the kernel log's mach clock)."""
import argparse, collections, json, statistics as st, sys

ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
ap.add_argument('trace', nargs='?')
ap.add_argument('--passes')
ap.add_argument('--t0', type=int, default=0)
ap.add_argument('--t1', type=int, default=1 << 62)
ap.add_argument('--worst', type=float, default=7.0)
ap.add_argument('--idle', type=float, default=250.0)
ap.add_argument('--rows', type=int, default=25)
ap.add_argument('--json')
a = ap.parse_args()


def passes(path):
    head, rows = '', []
    for line in open(path, errors='replace'):
        if line.startswith('#'):
            head = line
        elif line.startswith('key,') or not line.strip():
            continue
        else:
            f = line.rstrip('\n').rsplit(',', 7)
            if len(f) == 8:
                rows.append((f[0], int(f[1]), *map(float, f[2:6]), int(f[6]), int(f[7])))
    h = dict(zip(head.split()[2::2], head.split()[3::2])) if head else {}
    swaps = int(h.get('swaps', 0)) or 1
    window = (int(h.get('t1', 0)) - int(h.get('t0', 0))) / 1e6
    print(head.strip())
    print('%d swaps over %.1f s (%.1f swaps/s); events %s, unpaired %s' % (swaps, window, swaps / window if window > 0 else 0,
                                                                           h.get('events', '?'), h.get('dropped', '?')))
    nested = ('sysmem_clear', 'gmem_clear', 'gmem_load', 'gmem_store', 'sysmem_resolve', 'generic_clear', 'gmem_resolve')
    top = sum(r[2] for r in rows if not r[0].startswith(nested))
    print('%-46s %8s %9s %8s %8s %8s %7s %9s' % ('pass', 'n/frame', 'ms/frame', 'median', 'p95', 'max', 'share', 'draws'))
    for k, n, tot, med, p95, mx, dmed, dmax in rows:
        inner = k.startswith(nested)
        print('%-46s %8.2f %9.2f %8.3f %8.3f %8.2f %6s %9s' % (('  ' if inner else '') + k[:44], n / swaps, tot / swaps, med, p95, mx,
              '' if inner else '%5.1f%%' % (100 * tot / top if top else 0), '%d/%d' % (dmed, dmax) if k.startswith('rp ') else ''))
    print('total (top-level passes) %.2f ms per presented frame; nested steps (indented) are inside their render pass' % (top / swaps))


if a.passes:
    passes(a.passes)
    sys.exit(0)
if not a.trace:
    ap.error('a TRACE.csv or --passes FILE')

C = collections.namedtuple('C', 'seq queue created commit committed kstart kend gstart gend done handled passes draws dispatches blits pixels '
                                'pipe_us pipe_n wait_us flush_us flush_kib label')
cbs, waits, pipes, swaps = [], [], [], []
for line in open(a.trace, errors='replace'):
    f = line.rstrip('\n').split(',')
    try:
        if f[0] == 'C' and len(f) >= 23:
            cbs.append(C(*map(int, f[1:22]), ','.join(f[22:])))
        elif f[0] == 'W':
            waits.append(tuple(map(int, f[1:5])))            # start, us, kind, seq
        elif f[0] == 'P':
            pipes.append((int(f[1]), int(f[2]), int(f[3])))  # start, us, kind
        elif f[0] == 'S':
            swaps.append(tuple(map(int, f[1:10])))           # seq wait offer presented completed w h mapId result
    except ValueError:
        continue
if not swaps:
    sys.exit('frametrace: no swaps (S lines) in %s' % a.trace)
present_q = {c.queue for c in cbs if c.label == 'ngpu.present'}
ca = sorted((c for c in cbs if c.queue not in present_q), key=lambda c: c.commit)
pres = sorted((c for c in cbs if c.queue in present_q), key=lambda c: c.commit)
swaps = sorted((s for s in swaps if a.t0 <= s[2] <= a.t1), key=lambda s: s[2])
ms = lambda us: us / 1000.0


def between(items, lo, hi, key):
    return [x for x in items if lo < key(x) <= hi]


frames = []
for prev, s in zip(swaps, swaps[1:]):
    lo, hi = prev[2], s[2]
    if hi - lo > a.idle * 1000:
        continue
    mine = between(ca, lo, hi, lambda c: c.commit)
    copy = between(pres, s[2], s[3] + 1, lambda c: c.commit)
    fr = dict(offer=hi, ms=ms(hi - lo), cbs=len(mine), seq=s[0])
    if mine:
        last = max(mine, key=lambda c: c.handled)
        g = lambda c: c.gend - c.gstart if c.gend and c.gstart else 0
        fr.update(cpu_gap=ms(max(min(c.created for c in mine) - lo, 0)),
                  encode=ms(sum(c.commit - max(c.created, lo) for c in mine)),
                  pipe=ms(sum(c.pipe_us for c in mine)), wait=ms(sum(c.wait_us for c in mine)),
                  flush=ms(sum(c.flush_us for c in mine)), flush_kib=sum(c.flush_kib for c in mine),
                  commit=ms(sum(c.committed - c.commit for c in mine)),
                  gpu=ms(sum(g(c) for c in mine)),
                  gpu_queue=ms(max(last.gstart - last.committed, 0)) if last.gstart else 0,
                  complete=ms(max(last.done - last.gend, 0)) if last.gend else 0,
                  to_swap=ms(max(hi - last.handled, 0)),
                  passes=sum(c.passes for c in mine), draws=sum(c.draws for c in mine), dispatches=sum(c.dispatches for c in mine),
                  blits=sum(c.blits for c in mine), mpix=sum(c.pixels for c in mine) / 1e6,
                  labels=sorted({c.label for c in mine if c.label}))
    fr['present'] = ms(s[3] - s[2])
    fr['present_gpu'] = ms(sum(c.gend - c.gstart for c in copy if c.gend and c.gstart))
    fr['waits'] = [(w[2], ms(w[1])) for w in between(waits, lo, hi, lambda w: w[0]) if w[1] >= 500]
    fr['pipes'] = sum(1 for p in between(pipes, lo, hi, lambda p: p[0]))
    fr['pipe_all'] = ms(sum(p[1] for p in between(pipes, lo, hi, lambda p: p[0])))
    frames.append(fr)
if not frames:
    sys.exit('frametrace: no frames (every swap interval over --idle)')

t = sorted(f['ms'] for f in frames)
q = lambda p: t[min(len(t) - 1, int(p / 100 * len(t)))]
span = sum(t) / 1000
print('%d frames over %.1f s of swaps: %.1f fps | frame ms median %.1f p90 %.1f p95 %.1f max %.1f | >33 ms: %d | >50 ms: %d | C %d S %d W %d P %d'
      % (len(frames), span, len(frames) / span if span else 0, q(50), q(90), q(95), t[-1], sum(x > 33.3 for x in t), sum(x > 50 for x in t),
         len(cbs), len(swaps), len(waits), len(pipes)))
cut = q(100 - a.worst)
worst = sorted([f for f in frames if f['ms'] >= cut or f['ms'] > 33.3], key=lambda f: -f['ms'])
mid = [f for f in frames if q(40) <= f['ms'] <= q(60)]
keys = ['cpu_gap', 'encode', 'pipe', 'wait', 'commit', 'flush', 'gpu', 'gpu_queue', 'complete', 'to_swap', 'present', 'present_gpu', 'pipe_all']
print('\nworst %d frames (>= %.1f ms or > 33.3 ms):' % (len(worst), cut))
print('%9s %6s %3s ' % ('offer_us', 'ms', 'cb') + ' '.join('%8s' % k for k in keys) + '  passes draws disp blits Mpix  waits>0.5ms')
for f in worst[:a.rows]:
    print('%9d %6.1f %3d ' % (f['offer'] // 1000 % 1000000000, f['ms'], f['cbs']) + ' '.join('%8.2f' % f.get(k, 0) for k in keys) +
          '  %6s %5s %4s %5s %4.1f  %s' % (f.get('passes', ''), f.get('draws', ''), f.get('dispatches', ''), f.get('blits', ''), f.get('mpix', 0),
                                           ' '.join('%s%.1f' % ('wc' if k == 0 else 'ws', v) for k, v in f['waits'])))
mean = lambda fs, k: st.mean(f.get(k, 0) for f in fs) if fs else 0
print('\nranked causes: mean ms on the worst frames vs the median frames (40-60th pct, %d frames)' % len(mid))
rank = sorted(((mean(worst, k) - mean(mid, k), k) for k in keys if k not in ('pipe_all',)), reverse=True)
print('%12s %8s %8s %8s' % ('component', 'worst', 'median', 'delta'))
for d, k in rank:
    print('%12s %8.2f %8.2f %+8.2f' % (k, mean(worst, k), mean(mid, k), d))
print('%12s %8.1f %8.1f %+8.1f' % ('frame', mean(worst, 'ms'), mean(mid, 'ms'), mean(worst, 'ms') - mean(mid, 'ms')))
for k in ('passes', 'draws', 'dispatches', 'blits', 'mpix', 'cbs', 'flush_kib', 'pipes'):
    print('%12s %8.1f %8.1f' % (k, mean(worst, k), mean(mid, k)))
if a.json:
    json.dump(dict(frames=frames, worst=worst), open(a.json, 'w'), indent=1)
