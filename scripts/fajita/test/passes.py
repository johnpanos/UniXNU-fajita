#!/usr/bin/env python3
"""passes.py TRACE.csv [FRAMES]: Turnip u_trace print_csv -> GPU time per pass kind. Pairs start_X/end_X per batch, keys render passes by
size, tiling and draw count, and prints total GPU ms, count, mean, and ms per presented frame (FRAMES, default: the number of scanout
compute dispatches, i.e. presenter frames). Only the busiest window (from the first to the last event of the swipes, see --t0/--t1 ns) if given."""
import csv, re, sys, collections
path = sys.argv[1]; frames = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 0
def kv(s):
    return dict(re.findall(r'(\w+)=([^,\s]+)', s))
stacks = collections.defaultdict(list); agg = collections.defaultdict(lambda: [0, 0.0, 0]); spans = []
lines = 0; tmin = tmax = None
for row in csv.reader(open(path, errors='replace')):
    if len(row) < 4 or not row[2].isdigit(): continue
    lines += 1
    batch, ns, name = row[1], int(row[2]), row[3]; args = ','.join(row[4:])
    tmin = ns if tmin is None else min(tmin, ns); tmax = ns if tmax is None else max(tmax, ns)
    m = re.match(r'(start|end)_(\w+)', name)
    if not m: continue
    kind = m.group(2)
    if m.group(1) == 'start': stacks[(batch, kind)].append((ns, args)); continue
    if not stacks[(batch, kind)]: continue
    t0, a0 = stacks[(batch, kind)].pop(); d = ns - t0
    if d < 0 or d > 2e9: continue
    a = kv(a0); b = kv(args)
    if kind == 'render_pass':
        key = 'render_pass %sx%s %s draws~%s ubwc=%s' % (a.get('width'), a.get('height'), 'gmem' if b.get('tiledRender') == 'true' else 'sysmem',
              (lambda n: '1' if n <= 1 else '2-9' if n < 10 else '10-99' if n < 100 else '100+')(int(b.get('drawCount', '0') or 0)), a.get('ubwc', '?'))
    elif kind in ('compute', 'compute_indirect'):
        key = 'compute ' + ' '.join('%s=%s' % (k, a[k]) for k in sorted(a) if k.startswith(('group', 'num')))
    else:
        key = kind
    e = agg[key]; e[0] += 1; e[1] += d / 1e6; e[2] = max(e[2], d)
    if kind not in ('cmd_buffer',): spans.append((t0, ns))
if not frames:
    frames = max((v[0] for k, v in agg.items() if k.startswith('compute')), default=1)
wall = (tmax - tmin) / 1e9 if tmin is not None else 0
print('%d events, %.1f s of GPU timeline, frames %d' % (lines, wall, frames))
tot = sum(v[1] for k, v in agg.items() if k != 'cmd_buffer')
print('%-64s %7s %9s %8s %8s %6s' % ('pass', 'count', 'total ms', 'mean ms', 'ms/frame', 'share'))
for k, v in sorted(agg.items(), key=lambda kv_: -kv_[1][1])[:30]:
    print('%-64s %7d %9.1f %8.2f %8.2f %5.1f%%' % (k[:64], v[0], v[1], v[1] / v[0], v[1] / frames, 100 * v[1] / tot if k != 'cmd_buffer' and tot else 0))
