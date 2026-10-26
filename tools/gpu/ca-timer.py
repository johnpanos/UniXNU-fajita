#!/usr/bin/env python3
"""ca-timer.py SCENE.json...: why the render server skipped a display-timer tick. Reads the `ca` lines gfx-scene.py --cawatch recorded
(fjsurface --watch-ca: backboardd's IOMFBServer and IOMFBDisplay fields, polled every 200 us) and classifies every tick of CA's timer:

  RENDER     the tick rendered (an update was due: next-update time <= the tick's target, one interval ahead)
  CANT       it tried, but IOMFBDisplay::can_update failed (no swap slot free: both ring entries still waiting in SwapWait)
  THRESHOLD  vsync_callback dropped it: the tick was before the vbl threshold the previous render set (render end + one period on
             the immediate path, target - half a period on the vsync path)
  NOTDUE     nothing due yet: the next-update time (a client's commit) was after this tick's target, so CA only collected
  IDLE       no update pending at all (the timer counts down 3 such ticks, then stops)
  STOPPED    a gap after an idle tick: the timer had stopped (nothing due) until a commit restarted it; not a drop
  LATE       the timer itself skipped: the interval to the next tick was over 1.5 periods (the callback, which renders on the timer
             thread, ran more than a period late, and display_timer_callback re-phased from "now")
Model and evidence: NOTES-gfx.md "CA's display timer". Prints per scene the counts, then every gap of 2+ ticks between renders with the
classes of the ticks in it and the retire ring around it (ms since the last retire, ms to the next), whether CA's phase reference
(the timing shmem frame info feeds) was ever set, and the ticks' phase against the panel's vsync (rd_ptr) ring: a locked timer
fills one bin. And CA's per-swap FrameInfo records: a count that only grows means frame info never matches CA's swap id
(collect_frame_info takes the exact id and drops the older ones); no records at all means CA keeps none (D+0x9f2 bit 35 clear)."""
import bisect
import json
import math
import sys

TICKS_PER_S = 19.2e6
US_PER_TICK = 1e6 / TICKS_PER_S


def parse(lines):
    out = []
    for l in lines:
        f = l.split()
        try:
            d = dict(t=int(f[1]), tick=int(f[3]), thr=int(f[5]), next=float(f[7]), idle=int(f[9]), cant=int(f[11]),
                     ring=f[13], dl=int(f[15]), ref=tuple(int(x) for x in f[17:20]) if len(f) >= 20 else None,
                     fin=int(f[21]) if len(f) >= 25 else None, fid=int(f[22]) if len(f) >= 25 else None,
                     flags=int(f[24], 16) if len(f) >= 25 else None)
        except (IndexError, ValueError):
            continue
        out.append(d)
    return out


def classify(samples, period):
    """[(tick_time, class, detail)] in time order."""
    ticks, i = [], 0
    while i < len(samples):
        j = i
        while j < len(samples) and samples[j]['tick'] == samples[i]['tick']:
            j += 1
        ticks.append((i, j))
        i = j
    res = []
    for k, (i, j) in enumerate(ticks):
        T = samples[i]['tick']
        before = samples[i - 1] if i else samples[i]
        win = samples[i:j]
        target_s = (T + period) / TICKS_PER_S
        nxt = before['next']
        if before['thr'] > T or win[0]['thr'] > T and win[0]['thr'] == before['thr']:
            c = 'THRESHOLD'
        elif any(s['cant'] > before['cant'] for s in win):
            c = 'CANT'
        elif math.isinf(nxt):
            c = 'IDLE'
        elif nxt > target_s + 1e-4:
            c = 'NOTDUE'
        else:
            c = 'RENDER'
        nt = samples[j]['tick'] if j < len(samples) else None
        res.append((T, c, dict(next_ms=None if math.isinf(nxt) else (nxt - T / TICKS_PER_S) * 1e3,
                               thr_ms=(before['thr'] - T) * US_PER_TICK / 1e3, ring=win[0]['ring'],
                               to_next_ms=None if nt is None else (nt - T) * US_PER_TICK / 1e3)))
        if nt is not None and nt - T > 1.5 * period:
            # after an idle tick the timer was stopped (3 idle ticks, set_vsync_disabled) and restarted by a commit: not a late frame
            res.append((T + period, 'STOPPED' if c == 'IDLE' else 'LATE', dict(gap_ms=(nt - T) * US_PER_TICK / 1e3)))
    return res


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for path in sys.argv[1:]:
        rec = json.load(open(path))
        s = parse(rec.get('ca', []))
        if len(s) < 10:
            print('%s: no ca lines (gfx-scene.py --cawatch)%s' % (rec.get('scene', path), ' ' + str(rec.get('ca_errors')) if rec.get('ca_errors') else ''))
            continue
        tk = sorted({x['tick'] for x in s})
        d = sorted(b - a for a, b in zip(tk, tk[1:]) if b > a)
        period = d[len(d) // 2] if d else 320000
        res = classify(s, period)
        counts = {}
        for _, c, _ in res:
            counts[c] = counts.get(c, 0) + 1
        print('%s: %d ticks, period %.3f ms: %s' % (rec.get('scene', path), len(res), period * US_PER_TICK / 1e3,
                                                     ' '.join('%s %d' % kv for kv in sorted(counts.items()))))
        refs = {x['ref'] for x in s if x.get('ref')}
        print('  phase reference (timing shmem +0x28/+0x30/+0x38): %s' % (
            'never set (no frame info reached add_timing: the timer runs at its own phase)' if refs <= {(0, 0, 0)} else
            '%d distinct values, last %s' % (len(refs), s[-1]['ref'])))
        fi = [x for x in s if x.get('fin') is not None]
        if fi:
            ids = [x['fid'] for x in fi if x['fin']]
            c1 = rec.get('counters1') or {}
            print('  CA FrameInfo records waiting: max %d, oldest ids %s..%s; records kept (D+0x9f2 bit 35): %s; display sent %s frame infos, '
                  'last id %s, ids differing from the generation %s' % (
                      max(x['fin'] for x in fi), min(ids) if ids else '-', max(ids) if ids else '-',
                      'yes' if any(x['flags'] >> 35 & 1 for x in fi) else 'no',
                      (c1.get('fi_sent') or 0) - ((rec.get('counters0') or {}).get('fi_sent') or 0), c1.get('fi_last_id'), c1.get('fi_id_differs')))
        vs = rec.get('vsync', [])
        if vs:
            ph = []
            for T, c, _ in res:
                if c == 'LATE':
                    continue
                u = T * US_PER_TICK
                n = bisect.bisect_right(vs, u)
                if n:
                    ph.append(u - vs[n - 1])
            if ph:
                b = [0] * 8
                for x in ph:
                    b[min(7, int(x / (period * US_PER_TICK) * 8))] += 1
                print('  tick phase after the panel vsync (8 bins of a period): %s' % ' '.join(map(str, b)))
        retire = rec.get('retire', [])
        renders = [k for k, r in enumerate(res) if r[1] == 'RENDER']
        shown = 0
        for a, b in zip(renders, renders[1:]):
            if b - a < 2 or all(r[1] in ('IDLE', 'STOPPED') for r in res[a + 1:b]):
                continue
            T = res[a][0] * US_PER_TICK
            n = bisect.bisect_left(retire, T)
            last = T - retire[n - 1] if n else None
            nxt = retire[n] - T if n < len(retire) else None
            cls = ' '.join('%s(%s)' % (r[1], ','.join('%s=%.1f' % (k, v) for k, v in r[2].items() if isinstance(v, float)))
                           for r in res[a + 1:b])
            print('  render @%.1f ms, next render %d ticks later; between: %s | retire -%s/+%s ms' % (
                T / 1e3, b - a, cls, '%.1f' % (last / 1e3) if last is not None else '?', '%.1f' % (nxt / 1e3) if nxt is not None else '?'))
            shown += 1
            if shown >= 40:
                print('  ...')
                break
    return 0


if __name__ == '__main__':
    sys.exit(main())
