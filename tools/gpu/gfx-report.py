#!/usr/bin/env python3
"""gfx-report.py RUN_DIR [--refs DIR] [--quiet]: the gfx suite's report: one row per scene (scripts/fajita/test/gfx-suite.sh, one JSON per
scene from scripts/fajita/test/gfx-scene.py) -> RUN_DIR/report.txt and RUN_DIR/report.tsv.
gfx-report.py --compare RUN_A RUN_B [OUT.txt]: the two runs side by side (A/B: two builds, or two gpu-env variants in one boot).
gfx-report.py --noise RUN_DIR: scenes measured more than once in the run (SCENE, SCENE-r2, ...): the spread of each metric.

Speed, over the scene's motion windows only (a frame the render server did not present because nothing changed is not a drop):
  Inside a window, idle cadence is left out (idle gaps, counted): an interval over IDLE_VSYNCS (4) vsyncs between two long ones that
  ends in a small frame (the clock's 10 Hz while a drag has not moved anything yet); a long interval next to short ones is a stall.
  From SLOW_TAIL_MS (400) after the lift, a gap over 2.5 vsyncs ends the window too: a fling's slow tail moves less than a pixel a frame,
  so its frames repeat and are not presented. A window's frames start at the first two frames under 2.5 vsyncs apart: what came
  before (touch slop, gesture recognition, an app launch's first frame) is response time (resp), not frame time.
  A motion window starts at a touch step's start (a tap: its lift) and runs while the finger is down, then on after the lift while
  frames keep coming less than IDLE_MS (150) apart, for at most TAIL_MS (2000): a fling's or page snap's animation. A gap longer than
  IDLE_MS after the lift ends the motion; a gap of any length while the finger moves is a stall.
  frames     panel frames in the windows (the DPU's flip latches: what the panel showed)
  fps        frames per second of motion (intervals over their sum)
  p50/p99/p99.9  frame time percentiles (ms, between consecutive panel frames)
  1%low/0.1%low  fps of the average of the slowest 1% / 0.1% of frames (at least one frame: with under 1000 frames 0.1% is the worst)
  >16.7/>33  frames that took more than one vsync (1.5 periods) / more than two (2.5 periods)
  missed     vsyncs without a new frame (sum over intervals of round(interval / period) - 1)
  max        the longest frame time (ms); resp: touch-down to the first new frame (median ms; gesture recognition included)
Bottleneck, per scene:
  gpu%       GPU busy (RBBM status masked / always-on cycles) over the motion windows, from the governor's 20 ms samples; MHz the GPU
             clock measured the same way; temp the hottest tsens; perf: SP ALU and texture stall shares (kcmd gpu perf over the scene)
  cpu        the busiest threads of the render server (and the scene's app): % of one core and how often they were running when
             sampled, weighted by the CPU windows' overlap with motion; cores: the busiest core
  bound      vsync (>= 95% of frames on time); GPU (busy >= 85%, or the slow frames' GPU time is their largest part); CPU <thread> (a
             thread running >= 85% of samples); CPU render server (the slow frames' encode + commit, the Metal encode and translation, is
             their largest part); else pipeline: nothing saturated, the slow frames waited (cpu_gap: from the previous swap offer until
             the render server started the frame, i.e. waiting on the previous present, a vsync or the app's commit; present: offer to
             the presenter's copy returned)
Frame trace (NGPU_FRAME_TRACE=1, pulled by the suite into RUN_DIR/frametrace/): per render-server frame in the windows, the median
cpu_gap / encode / commit / gpu / present (ms), and the same on the slow frames (> 1.5 vsyncs).
Correctness: every shot step's screenshot against REFS/<scene>-<name>.png (default $FAJITA_WORK/gfx/refs; a repeat SCENE-r2 against
SCENE's), below the status bar: pixels differing by more than 8, and strongly (more than 96: an icon, text or a block missing or moved,
not a blur's or a gradient's rounding); DIFF when the strong ones are over 0.2% of the area. Stability: RUN_DIR/stability.json (the suite: CRASH rows, swap_wait
timeouts, GPU faults from the run's log) and the scene counters (hand-offs reclaimed, retires lost, flips refused, GPU IRQ errors)."""
import argparse
import glob
import json
import math
import os
import re
import statistics as st
import sys

IDLE_MS = float(os.environ.get('IDLE_MS', 150))
TAIL_MS = float(os.environ.get('TAIL_MS', 2000))
TAP_FIRST_MS = float(os.environ.get('TAP_FIRST_MS', 1000))
IDLE_VSYNCS = float(os.environ.get('IDLE_VSYNCS', 4))
SLOW_TAIL_MS = float(os.environ.get('SLOW_TAIL_MS', 400))
TV_US = 16684.0   # the panel's vsync period (59.94 Hz), measured per scene in scene_metrics
HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.environ.get('FAJITA_WORK') or os.path.join(HERE, '..', '..', '.cache', 'fajita-work')


def pct(v, p):
    if not v:
        return 0.0
    s = sorted(v)
    k = (len(s) - 1) * p / 100
    f = math.floor(k)
    return s[f] if f == k else s[f] + (s[min(f + 1, len(s) - 1)] - s[f]) * (k - f)


def low(v, frac):
    """fps of the mean of the slowest frac of frame times (at least one frame)."""
    if not v:
        return 0.0
    s = sorted(v, reverse=True)
    n = max(1, int(math.ceil(len(s) * frac)))
    return 1000.0 / st.mean(s[:n])


def windows(rec):
    """Motion windows [(start_us, end_us, kind, first_frame_us)] and the panel frames in each."""
    pres = rec.get('latch') or rec.get('retire') or []
    src = 'latch' if rec.get('latch') else 'retire' if rec.get('retire') else 'none'
    steps = [s for s in rec['steps'] if s.get('kind') == 'touch']
    allsteps = [s for s in rec['steps'] if 'start_us' in s]
    out = []
    for s in steps:
        tap = s['spec'].startswith('tap')
        ws = s['end_us'] if tap else s['start_us']
        later = [x['start_us'] for x in allsteps if x['start_us'] > s['end_us'] + 1]
        hard = min(later) if later else float('inf')
        frames, prev = [], None
        for p in pres:
            if p < ws:
                continue
            if p >= hard:
                break
            if prev is None:
                if not tap and p > s['end_us'] + IDLE_MS * 1000 or tap and p > ws + TAP_FIRST_MS * 1000:
                    break
            elif p > s['end_us'] and (p - prev > IDLE_MS * 1000 or p > s['end_us'] + TAIL_MS * 1000):
                break
            elif p > s['end_us'] + SLOW_TAIL_MS * 1000 and p - prev > 2.5 * TV_US:
                break   # a fling's slow tail: moves under a pixel a frame, so frames repeat and are not presented (not drops)
            frames.append(p)
            prev = p
        # The motion starts at the first of two frames under 2.5 vsyncs apart: before that (the touch slop, gesture recognition, an app
        # launch's first frame) is response time, not frame time.
        k = 0
        while k + 1 < len(frames) and frames[k + 1] - frames[k] > 2.5 * TV_US:
            k += 1
        resp = (frames[k] - ws) / 1000 if frames else None
        frames = frames[k:]
        out.append(dict(spec=s['spec'], start=ws, end=frames[-1] if frames else ws, lift=s['end_us'], frames=frames, resp=resp))
    return out, src


def vsync_period(rec):
    v = rec.get('vsync') or []
    d = [b - a for a, b in zip(v, v[1:]) if 10000 < b - a < 25000]
    return st.median(d) if len(d) > 10 else 16685.0


def overlap(a0, a1, b0, b1):
    return max(0.0, min(a1, b1) - max(a0, b0))


def frametrace(paths):
    """Render-server frames from NativeMetal frame traces: [(offer_us, dict of components ms)]."""
    frames = []
    for path in paths:
        cbs, swaps = [], []
        for line in open(path, errors='replace'):
            f = line.rstrip('\n').split(',')
            try:
                if f[0] == 'C' and len(f) >= 23:
                    cbs.append(dict(zip(('seq queue created commit committed kstart kend gstart gend done handled passes draws dispatches '
                                         'blits pixels pipe_us pipe_n wait_us flush_us flush_kib').split(), map(int, f[1:22])), label=','.join(f[22:])))
                elif f[0] == 'S':
                    swaps.append(tuple(map(int, f[1:10])))
                elif f[0] == 'Q' and len(f) >= 9:
                    PRESENT_Q.append(tuple(map(int, f[1:9])))
            except ValueError:
                continue
        if not swaps:
            continue
        pq = {c['queue'] for c in cbs if c['label'] == 'ngpu.present'}
        ca = sorted((c for c in cbs if c['queue'] not in pq), key=lambda c: c['commit'])
        swaps.sort(key=lambda s: s[2])
        j = 0
        for prev, s in zip(swaps, swaps[1:]):
            lo, hi = prev[2], s[2]
            while j < len(ca) and ca[j]['commit'] <= lo:
                j += 1
            mine = []
            k = j
            while k < len(ca) and ca[k]['commit'] <= hi:
                mine.append(ca[k])
                k += 1
            fr = dict(ms=(hi - lo) / 1000, present=(s[3] - s[2]) / 1000, cbs=len(mine))
            if mine:
                last = max(mine, key=lambda c: c['handled'])
                fr.update(cpu_gap=max(min(c['created'] for c in mine) - lo, 0) / 1000,
                          encode=sum(c['commit'] - max(c['created'], lo) for c in mine) / 1000,
                          commit=sum(c['committed'] - c['commit'] for c in mine) / 1000,
                          gpu=sum(c['gend'] - c['gstart'] for c in mine if c['gend'] and c['gstart']) / 1000,
                          gpu_queue=max(last['gstart'] - last['committed'], 0) / 1000 if last['gstart'] else 0,
                          to_swap=max(hi - last['handled'], 0) / 1000, draws=sum(c['draws'] for c in mine),
                          passes=sum(c['passes'] for c in mine))
            frames.append((hi, fr))
    return frames


PRESENT_Q = []   # the presenter's stages per swap (frame trace Q lines): seq offered committed waitBegan waitEnded returned completeBegan completeEnded
FT_KEYS = ('cpu_gap', 'encode', 'commit', 'gpu', 'gpu_queue', 'to_swap', 'present')


def scene_metrics(rec, ft_frames, refs):
    m = dict(scene=rec['scene'])
    tv = vsync_period(rec)
    ws, src = windows(rec)
    m['source'] = src
    m['vsync_ms'] = tv / 1000
    tvm = tv / 1000
    # Idle cadence inside a window: a finger can be down while nothing moves (a drag that has not passed the touch slop, a list at its
    # end), and then the only frames are the idle ones (the home screen's clock, 10 Hz). An interval over IDLE_VSYNCS (4) vsyncs whose
    # neighbours are long too is that cadence, not a stall; with a frame trace it must also end in a small frame (under 40% of the
    # scene's median draws). A stall is a long interval next to short ones. Idle intervals are left out and counted (idle_gaps).
    offers = sorted(ft_frames)
    otimes = [t for t, _ in offers]
    med_draws = st.median([f.get('draws', 0) for t, f in offers if any(w['start'] <= t <= w['end'] for w in ws)] or [0])
    def small(t):
        import bisect
        i = bisect.bisect_right(otimes, t) - 1
        if i < 0 or t - otimes[i] > 60000:
            return not offers
        return offers[i][1].get('draws', 0) < 0.4 * med_draws
    iv, idle = [], []
    for w in ws:
        f = w['frames']
        d = [(b - a) / 1000 for a, b in zip(f, f[1:])]
        for i, x in enumerate(d):
            long_ = lambda j: j < 0 or j >= len(d) or d[j] > IDLE_VSYNCS * tvm
            if x > IDLE_VSYNCS * tvm and long_(i - 1) and long_(i + 1) and small(f[i + 1]):
                idle.append(x)
            else:
                iv.append(x)
    m['idle_gaps'], m['idle_s'] = len(idle), sum(idle) / 1000
    m['windows'] = len(ws)
    m['iv'] = iv
    m['frames'] = sum(len(w['frames']) for w in ws)
    m['motion_s'] = sum(iv) / 1000
    m['fps'] = len(iv) / (sum(iv) / 1000) if iv else 0
    m['p50'], m['p99'], m['p999'] = pct(iv, 50), pct(iv, 99), pct(iv, 99.9)
    m['low1'], m['low01'] = low(iv, 0.01), low(iv, 0.001)
    m['over16'] = sum(1 for x in iv if x > 1.5 * tvm)
    m['over33'] = sum(1 for x in iv if x > 2.5 * tvm)
    m['missed'] = sum(max(0, round(x / tvm) - 1) for x in iv)
    m['max'] = max(iv) if iv else 0
    rs = [w['resp'] for w in ws if w['resp'] is not None]
    m['resp'] = st.median(rs) if rs else 0
    m['empty_windows'] = [w['spec'] for w in ws if len(w['frames']) < 3]
    m['ontime'] = 1 - m['over16'] / len(iv) if iv else 0
    # Pacing: where each retired swap lands against the panel's frame starts (rd_ptr), over the motion windows. A display timer locked
    # to the panel retires its frames at a steady phase; a free-running one drifts through the whole period.
    import bisect
    vs = rec.get('vsync') or []
    phases = []
    for x in rec.get('retire') or []:
        if any(w['start'] <= x <= w['end'] for w in ws):
            i = bisect.bisect_left(vs, x)
            if 0 < i < len(vs):
                phases.append((vs[i] - x) / 1000)
    if len(phases) > 10:
        hist = [0] * 8
        for ph in phases:
            hist[min(7, int(ph / tvm * 8))] += 1
        m['phase_hist'] = hist
        m['phase_spread'] = pct(phases, 90) - pct(phases, 10)
    # GPU over the motion windows (each governor sample covers the 20 ms before its stamp)
    gs = [g for g in rec.get('gov', []) if any(w['start'] < g['us'] <= w['end'] + 20000 for w in ws)]
    busy, alw = sum(g['busy'] for g in gs), sum(g['always'] for g in gs)
    m['gpu_busy'] = 100.0 * busy / alw if alw else 0
    span = 20000 * len(gs)
    m['gpu_mhz'] = alw / span if span else 0
    m['gpu_busy_peak'] = max((100.0 * g['busy'] / g['always'] for g in gs if g['always']), default=0)
    lv = {}
    for g in gs:
        lv[g['level']] = lv.get(g['level'], 0) + 1
    m['gpu_levels'] = ' '.join('L%d:%d%%' % (k, 100 * v // max(len(gs), 1)) for k, v in sorted(lv.items()))
    m['temp'] = max((g['temp'] for g in rec.get('gov', [])), default=0) / 10
    m['gpu_cap'] = min((g['cap'] for g in rec.get('gov', [])), default=0)
    p = rec.get('perf') or {}
    alw_p = p.get('rbbm_always', 0)
    m['perf_busy'] = 100.0 * p.get('gpu_busy', 0) / alw_p if alw_p else 0
    m['sp_alu'] = 100.0 * p.get('sp_alu', 0) / 2 / p['sp_busy'] if p.get('sp_busy') else 0
    m['sp_stall_tp'] = 100.0 * p.get('sp_stall_tp', 0) / p['sp_busy'] if p.get('sp_busy') else 0
    m['tp_stall_uche'] = 100.0 * p.get('tp_stall_uche', 0) / p['tp_busy'] if p.get('tp_busy') else 0
    m['uche_lat'] = p.get('uche_vbif_lat_cyc', 0) / p['uche_vbif_lat_n'] if p.get('uche_vbif_lat_n') else 0
    # CPU: windows weighted by their overlap with motion
    procs, threads, cores, wsum = {}, {}, [], 0.0
    for c in rec.get('cpu', []):
        c0, c1 = c['start_us'], c['start_us'] + c['ms'] * 1000
        ov = sum(overlap(c0, c1, w['start'], w['end']) for w in ws) / max(c1 - c0, 1)
        if ov <= 0:
            continue
        wsum += ov
        for pr in c['procs']:
            k = pr['name']
            procs[k] = procs.get(k, 0) + ov * pr['ms'] / c['ms'] * 100
        for pid, ts in c['threads'].items():
            pname = next((pr['name'] for pr in c['procs'] if str(pr['pid']) == pid), pid)
            for t in ts:
                k = '%s:%s' % (pname, t['name'] if t['name'] != '-' else t['tid'])
                e = threads.setdefault(k, [0.0, 0.0, 0])
                e[0] += ov * t['ms'] / c['ms'] * 100
                e[1] += ov * t['run']
                e[2] = t['pri']
        if c['cores']:
            cores.append((ov, c['cores']))
    if wsum:
        m['procs'] = sorted(((v / wsum, k) for k, v in procs.items()), reverse=True)[:6]
        m['threads'] = sorted(((v[0] / wsum, v[1] / wsum, k, v[2]) for k, v in threads.items()), reverse=True)[:6]
        n = max(len(cc) for _, cc in cores) if cores else 0
        m['cores'] = [sum(ov * (cc[i][0] + cc[i][1]) for ov, cc in cores if i < len(cc)) / sum(ov for ov, _ in cores) for i in range(n)] if cores else []
    else:
        m['procs'], m['threads'], m['cores'] = [], [], []
    top = m['threads'][0] if m['threads'] else None
    m['cpu_top'] = '%s %.0f%% (run %.0f%%)' % (top[2], top[0], top[1]) if top else '-'
    m['cpu_top_pct'], m['cpu_top_run'] = (top[0], top[1]) if top else (0, 0)
    # frame trace in the windows
    # frames whose previous offer is over 250 ms back follow an idle stretch (frametrace.py --idle): not frames of the motion
    fts = [fr for t, fr in ft_frames if fr['ms'] <= 250 and any(w['start'] <= t <= w['end'] + 1000 for w in ws)]
    m['ft_frames'] = len(fts)
    slow = [fr for fr in fts if fr['ms'] > 1.5 * tvm and 'gpu' in fr]
    full = [fr for fr in fts if 'gpu' in fr]
    m['ft_med'] = {k: st.median(fr.get(k, 0) for fr in full) for k in FT_KEYS} if full else {}
    m['ft_slow'] = {k: st.mean(fr.get(k, 0) for fr in slow) for k in FT_KEYS} if slow else {}
    m['ft_slow_n'] = len(slow)
    qs = [q for q in PRESENT_Q if all(q[1:]) and any(w['start'] <= q[1] <= w['end'] for w in ws)]
    if qs:
        stage = lambda a, b: st.median((q[b] - q[a]) / 1000 for q in qs)
        p90 = lambda a, b: pct([(q[b] - q[a]) / 1000 for q in qs], 90)
        m['present_stages'] = ('offer->committed %.2f, ->wait %.2f, wait %.2f (p90 %.2f), ->returned %.2f (p90 %.2f), ->complete call %.2f, '
                               'complete %.2f (p90 %.2f) ms' % (stage(1, 2), stage(2, 3), stage(3, 4), p90(3, 4), stage(4, 5), p90(4, 5),
                                                                stage(5, 6), stage(6, 7), p90(6, 7)))
    # bound
    why = ''
    sl = m['ft_slow']
    work = sl.get('encode', 0) + sl.get('commit', 0)
    if iv and m['ontime'] >= 0.95:
        bound = 'vsync'
    elif m['gpu_busy'] >= 85:
        bound, why = 'GPU', 'busy %.0f%%' % m['gpu_busy']
    elif top and top[1] >= 85:
        bound, why = 'CPU %s' % top[2], 'running %.0f%% of samples' % top[1]
    elif slow and sl.get('gpu', 0) > max(work, sl.get('cpu_gap', 0), tvm):
        bound, why = 'GPU', 'slow frames: gpu %.1f ms' % sl['gpu']
    elif slow and work > max(sl.get('gpu', 0), sl.get('cpu_gap', 0), tvm):
        bound, why = 'CPU render server', 'slow frames: encode+commit %.1f ms (gpu %.1f)' % (work, sl.get('gpu', 0))
    else:
        # nothing saturated: the render server waited (for the previous swap's present, a vsync, or the app's commit)
        bound = 'pipeline'
        why = ('slow frames wait %.1f ms before their work starts (cpu_gap), present %.1f ms; ' % (sl.get('cpu_gap', 0), sl.get('present', 0))
               if slow else '') + 'gpu %.0f%%, top thread %s' % (m['gpu_busy'], m['cpu_top'])
    m['bound'], m['bound_why'] = bound, why
    # correctness
    m['shots'] = []
    for s in rec['steps']:
        if s.get('kind') != 'shot':
            continue
        name = os.path.basename(s['path'])
        ref = os.path.join(refs, re.sub(r'-r\d+-', '-', name))   # a repeat (SCENE-r2) is compared with SCENE's reference
        d = diff(ref, s['path']) if os.path.exists(ref) and os.path.exists(s['path']) else None
        m['shots'].append((name, d))
    # stability counters over the scene
    c0, c1 = rec.get('counters0', {}), rec.get('counters1', {})
    dd = lambda k: (c1.get(k) or 0) - (c0.get(k) or 0) if c1.get(k) is not None and c0.get(k) is not None else None
    m['reclaimed'], m['retire_lost'], m['flips_refused'] = dd('reclaimed'), dd('retire_lost'), dd('flips_refused')
    m['gpu_fallbacks'], m['failures'], m['scanned_writes'] = dd('gpu_fallbacks'), dd('failures'), dd('scanned_writes')
    m['slot_busy'], m['ring_full'] = dd('slot_busy'), dd('ring_full')
    m['peak_jobs'] = c1.get('peak_jobs')
    m['irq_error'] = '%x' % c1['irq_first_error'] if c1.get('irq_first_error') else ''
    m['overflow'] = rec.get('overflow', {})
    m['step_errors'] = [s['spec'] + ': ' + s['error'] for s in rec['steps'] if 'error' in s]
    return m


def diff(a, b, box=(0, 60, 540, 1170)):
    from PIL import Image, ImageChops
    x = Image.open(a).convert('RGB').crop(box)
    y = Image.open(b).convert('RGB').crop(box)
    if x.size != y.size:
        return dict(px=-1, strong=-1, max=-1, total=0)
    d = ImageChops.difference(x, y)
    px = [max(p) for p in (d.get_flattened_data() if hasattr(d, 'get_flattened_data') else d.getdata())]
    return dict(px=sum(1 for p in px if p > 8), strong=sum(1 for p in px if p > 96), max=max(px), total=len(px))


COLS = [('frames', '%6d'), ('fps', '%5.1f'), ('p50', '%5.1f'), ('p99', '%5.1f'), ('p999', '%6.1f'), ('low1', '%5.1f'), ('low01', '%6.1f'),
        ('over16', '%5d'), ('over33', '%4d'), ('missed', '%6d'), ('max', '%6.1f'), ('resp', '%5.0f'), ('gpu_busy', '%4.0f'),
        ('gpu_mhz', '%4.0f')]
HEAD = ('scene', 'frames', 'fps', 'p50', 'p99', 'p99.9', '1%low', '0.1%low', '>16.7', '>33', 'missed', 'max', 'resp', 'gpu%', 'MHz')


def load(run):
    recs = []
    for p in sorted(glob.glob(os.path.join(run, '*.json'))):
        if os.path.basename(p) in ('stability.json', 'metrics.json'):
            continue
        try:
            r = json.load(open(p))
        except ValueError:
            continue
        if 'steps' in r:
            recs.append(r)
    return recs


def report(run, refs, quiet=False):
    recs = load(run)
    if not recs:
        sys.exit('gfx-report: no scene JSON in %s' % run)
    ft = frametrace(sorted(glob.glob(os.path.join(run, 'frametrace', '*.csv'))))
    ms = [scene_metrics(r, ft, refs) for r in recs]
    order = {r['scene']: r.get('t0_us') or 0 for r in recs}
    ms.sort(key=lambda m: order[m['scene']])
    stab = json.load(open(os.path.join(run, 'stability.json'))) if os.path.exists(os.path.join(run, 'stability.json')) else {}
    L = []
    meta = json.load(open(os.path.join(run, 'meta.json'))) if os.path.exists(os.path.join(run, 'meta.json')) else {}
    L.append('gfx suite %s %s' % (os.path.basename(os.path.abspath(run)), ' '.join('%s=%s' % kv for kv in sorted(meta.items()) if kv[0] not in ('env', 'start', 'diag'))))
    if meta.get('start'):
        L.append('start: %s%s' % (meta['start'], '' if meta.get('settled', 1) else '  !! NOT SETTLED'))
    if meta.get('diag'):
        L.append('!! DIAG RUN (a diagnostic was on; timings are not representative): %s' % meta['diag'])
    if meta.get('env'):
        L.append('gpu-env: ' + ' '.join(meta['env']))
    L.append('vsync %.3f ms (%.2f Hz); motion windows: touch to IDLE_MS %.0f after the lift (TAIL_MS %.0f); frames: panel latches' % (
        ms[0]['vsync_ms'], 1000 / ms[0]['vsync_ms'], IDLE_MS, TAIL_MS))
    L.append('')
    L.append('%-14s ' % HEAD[0] + ' '.join('%*s' % (len(f % 0), h) for (k, f), h in zip(COLS, HEAD[1:])) + '  bound')
    allv = []
    for m in ms:
        L.append('%-14s ' % m['scene'][:14] + ' '.join(f % m[k] for k, f in COLS) + '  ' + m['bound'])
    # every motion frame of the run pooled: the 0.1% low over thousands of frames instead of one scene's hundreds
    iv = [x for m in ms for x in m['iv']]
    if iv:
        tvm = ms[0]['vsync_ms']
        a = dict(frames=sum(m['frames'] for m in ms), fps=len(iv) / (sum(iv) / 1000), p50=pct(iv, 50), p99=pct(iv, 99), p999=pct(iv, 99.9),
                 low1=low(iv, 0.01), low01=low(iv, 0.001), over16=sum(1 for x in iv if x > 1.5 * tvm), over33=sum(1 for x in iv if x > 2.5 * tvm),
                 missed=sum(max(0, round(x / tvm) - 1) for x in iv), max=max(iv), resp=st.median([m['resp'] for m in ms]),
                 gpu_busy=st.mean(m['gpu_busy'] for m in ms), gpu_mhz=st.mean(m['gpu_mhz'] for m in ms))
        L.append('%-14s ' % 'ALL' + ' '.join(f % a[k] for k, f in COLS) + '  (every scene\'s motion frames pooled)')
    L.append('')
    for m in ms:
        L.append('%s: %d windows, %.1f s of motion, %.0f%% of frames on time, %d idle gaps (%.1f s) left out; source %s%s' % (
            m['scene'], m['windows'], m['motion_s'], 100 * m['ontime'], m['idle_gaps'], m['idle_s'], m['source'],
            ' | !! windows with no motion: %s' % ' '.join(m['empty_windows']) if m['empty_windows'] else ''))
        L.append('  bound: %s%s' % (m['bound'], ' (%s)' % m['bound_why'] if m['bound_why'] else ''))
        L.append('  gpu: busy %.0f%% (peak 20 ms %.0f%%) at %.0f MHz, levels %s, cap %s, max %.1f C | scene perf: busy %.0f%% alu %.0f%% of SP busy, '
                 'SP stall on TP %.0f%%, TP stall on UCHE %.0f%%, VBIF latency %.0f cycles' % (
                     m['gpu_busy'], m['gpu_busy_peak'], m['gpu_mhz'], m['gpu_levels'], m['gpu_cap'], m['temp'], m['perf_busy'], m['sp_alu'],
                     m['sp_stall_tp'], m['tp_stall_uche'], m['uche_lat']))
        if m['threads']:
            L.append('  cpu threads: ' + ' | '.join('%s %.0f%% run %.0f%% pri %d' % (k, c, r, p) for c, r, k, p in m['threads'][:4]))
            L.append('  cpu procs: ' + ' | '.join('%s %.0f%%' % (k, c) for c, k in m['procs'][:5]) + ' | cores ' +
                     ' '.join('%.0f' % c for c in m['cores']) + ' %')
        if m['ft_med']:
            L.append('  frame trace (%d frames, %d slow): median %s' % (m['ft_frames'], m['ft_slow_n'], ' '.join('%s %.1f' % (k, v) for k, v in m['ft_med'].items())))
            if m['ft_slow']:
                L.append('    slow frames: ' + ' '.join('%s %.1f' % (k, v) for k, v in m['ft_slow'].items()))
            if m.get('present_stages'):
                L.append('    presenter (median): ' + m['present_stages'])
        if m.get('phase_hist'):
            L.append('  pacing: retire-to-next-frame-start spread (p10-p90) %.1f ms of %.1f; by eighths of the period %s' % (
                m['phase_spread'], m['vsync_ms'], m['phase_hist']))
        if m.get('slot_busy') is not None:
            L.append('  job ring: submissions refused for a job slot %s, for ring space %s (retried by the client: waits), most jobs outstanding %s of 8' % (
                m['slot_busy'], m['ring_full'], m['peak_jobs']))
        sh = []
        for name, d in m['shots']:
            sh.append('%s %s' % (name, 'no ref' if d is None else ('%d px, %d strong%s' % (d['px'], d['strong'], ' DIFF' if d['total'] and d['strong'] > 0.002 * d['total'] else ''))))
        if sh:
            L.append('  shots: ' + ', '.join(sh))
        st_bad = ['%s %s' % (k, m[k]) for k in ('reclaimed', 'retire_lost', 'flips_refused', 'gpu_fallbacks', 'failures', 'scanned_writes') if m.get(k)]
        if st_bad or m['irq_error'] or m['overflow'] or m['step_errors']:
            L.append('  !! ' + ' '.join(st_bad) + (' gpu irq error %s' % m['irq_error'] if m['irq_error'] else '') +
                     (' overflow %s' % m['overflow'] if m['overflow'] else '') + (' step errors: %s' % '; '.join(m['step_errors']) if m['step_errors'] else ''))
    # the run as a whole: every motion frame pooled
    L.append('')
    if stab:
        L.append('stability: ' + ', '.join('%s %s' % kv for kv in stab.items() if not isinstance(kv[1], (list, dict))))
        for k, v in stab.items():
            if isinstance(v, list) and v:
                L.append('  %s: %s' % (k, '; '.join(str(x) for x in v[:8])))
    txt = '\n'.join(L) + '\n'
    open(os.path.join(run, 'report.txt'), 'w').write(txt)
    keys = ['scene'] + [k for k, _ in COLS] + ['motion_s', 'ontime', 'gpu_busy_peak', 'temp', 'perf_busy', 'sp_alu', 'sp_stall_tp',
                                               'tp_stall_uche', 'cpu_top', 'cpu_top_pct', 'cpu_top_run', 'bound', 'ft_slow_n']
    with open(os.path.join(run, 'report.tsv'), 'w') as f:
        f.write('\t'.join(keys) + '\tshots_px\n')
        for m in ms:
            f.write('\t'.join(('%.3f' % m[k]) if isinstance(m[k], float) else str(m[k]) for k in keys) + '\t' +
                    ','.join(str(d['px']) if d else '-' for _, d in m['shots']) + '\n')
    json.dump([{k: v for k, v in m.items() if k != 'iv'} for m in ms], open(os.path.join(run, 'metrics.json'), 'w'), indent=1, default=str)
    if not quiet:
        sys.stdout.write(txt)
    return ms


KEYM = [('fps', '%.1f'), ('p99', '%.1f'), ('low1', '%.1f'), ('low01', '%.1f'), ('over16', '%d'), ('over33', '%d'), ('max', '%.0f'),
        ('gpu_busy', '%.0f')]


def compare(a, b, out, refs):
    ma = {m['scene']: m for m in report(a, refs, quiet=True)}
    mb = {m['scene']: m for m in report(b, refs, quiet=True)}
    L = ['A: %s\nB: %s' % (a, b), '%-14s ' % 'scene' + ' '.join('%17s' % k for k, _ in KEYM)]
    for s in list(ma) + [s for s in mb if s not in ma]:
        x, y = ma.get(s), mb.get(s)
        if not x or not y:
            L.append('%-14s only in %s' % (s, 'A' if x else 'B'))
            continue
        L.append('%-14s ' % s[:14] + ' '.join('%17s' % ('%s -> %s' % (f % x[k], f % y[k])) for k, f in KEYM))
    txt = '\n'.join(L) + '\n'
    if out:
        open(out, 'w').write(txt)
    sys.stdout.write(txt)


def noise(run, refs):
    ms = report(run, refs, quiet=True)
    groups = {}
    for m in ms:
        groups.setdefault(re.sub(r'-r\d+$', '', m['scene']), []).append(m)
    L = ['run-to-run spread within %s (scenes measured more than once): value per repeat, and (max-min)/mean' % run]
    for s, g in groups.items():
        if len(g) < 2:
            continue
        parts = []
        for k, f in KEYM:
            v = [m[k] for m in g]
            mean = st.mean(v)
            parts.append('%s %s (%.0f%%)' % (k, '/'.join(f % x for x in v), 100 * (max(v) - min(v)) / mean if mean else 0))
        L.append('%-14s %s' % (s, ' | '.join(parts)))
    txt = '\n'.join(L) + '\n'
    open(os.path.join(run, 'noise.txt'), 'w').write(txt)
    sys.stdout.write(txt)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('run', nargs='?')
    ap.add_argument('--refs', default=os.path.join(WORK, 'gfx', 'refs'))
    ap.add_argument('--compare', nargs='+', metavar='RUN')
    ap.add_argument('--noise', metavar='RUN')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    if a.compare:
        if len(a.compare) < 2:
            ap.error('--compare RUN_A RUN_B [OUT]')
        return compare(a.compare[0], a.compare[1], a.compare[2] if len(a.compare) > 2 else None, a.refs)
    if a.noise:
        return noise(a.noise, a.refs)
    if not a.run:
        ap.error('RUN_DIR')
    report(a.run, a.refs, a.quiet)


if __name__ == '__main__':
    sys.exit(main())
