#!/usr/bin/env python3
"""swipe-rate.py [DIR[:FRAMES] | xy,X0,Y0,X1,Y1[:FRAMES] ...]: fire each on-demand swipe through the hub (kcmd touch swipe DIR), sample the GPU around it
(kcmd gpu raw), and print per swipe: presented frames (hand-off completions), fps, submissions, per-submit total/publish avg+max,
and the submit interval distribution. Window: from the swipe's start until SETTLE s after its lift."""
import os
import subprocess, sys, time, re, statistics as st

HUB = ['python3', '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'fajita-usbdbg-hub.py'), 'call', '--timeout', '8']
SETTLE = float(os.environ.get('SETTLE', '1.0'))


def call(*a):
    r = subprocess.run(HUB + list(a), capture_output=True, text=True, timeout=15)
    return r.stdout


def raw(frm):
    for attempt in range(3):   # a kcmd that gets no reply is retried, then the run stops with the reply it got
        out = call('kcmd', 'gpu', 'raw', str(frm))
        lines = out.strip().splitlines()
        head = next((l for l in lines if l.startswith('now ')), None)
        if head: break
        time.sleep(1)
    else:
        sys.exit('swipe-rate: kcmd gpu raw gave no "now" line after 3 tries: %r' % out.strip()[:200])
    nums = [int(x) for x in re.findall(r'\d+', head)]
    # now n retire wc publish tlb ring total max | imp own | unc | offers done | copies
    d = dict(now=nums[0], n=nums[1], publish=nums[4], total=nums[7], imp=nums[9], own=nums[10], unc=nums[11], offers=nums[12],
             done=nums[13], copies=nums[14], retire=nums[2], acq=nums[15] if len(nums)>15 else 0, acqskip=nums[16] if len(nums)>16 else 0)
    ring = {}
    for l in lines:
        m = re.fullmatch(r'(\d+) (\d+) (\d+) (\d+)(?: (\d+) (\d+))?', l.strip())
        if m:
            k, t0, tot, pub = map(int, m.groups()[:4])
            ring[k] = (t0, tot, pub, int(m.group(5) or 0), int(m.group(6) or 0))
    return d, ring


def touch(*a):
    return call('kcmd', 'touch', *a)


def one(spec):
    d, _, f = spec.partition(':')
    a0, ring = raw(0)
    # xy,X0,Y0,X1,Y1[:FRAMES]: a drag between two panel points (kcmd touch swipe xy ...), e.g. a list scroll
    q = touch('swipe', *(['xy'] + d.split(',')[1:] if d.startswith('xy,') else [d]), *([f] if f else []))
    m = re.search(r'queued (\d+)', q)
    if not m:
        print(f'{spec}: not queued: {q.strip()}')
        return None
    gid = int(m.group(1))
    t_lift = None
    allring = dict(ring)
    last = a0
    deadline = time.time() + 30
    while time.time() < deadline:
        s = touch()
        md = re.search(r'done (\d+)', s)
        if md and int(md.group(1)) >= gid and t_lift is None:
            t_lift = time.time()
        r = raw(max(last['n'] - 8, 0))
        if r:
            last, rg = r
            allring.update(rg)
        if t_lift and time.time() - t_lift >= SETTLE:
            break
        time.sleep(0.2)
    b = last
    win = (b['now'] - a0['now']) / 1e6
    ks = sorted(k for k in allring if k > a0['n'] and k <= b['n'])
    tot = [allring[k][1] for k in ks]
    pub = [allring[k][2] for k in ks]
    starts = [allring[k][0] for k in ks]
    gaps = [(y - x) / 1000 for x, y in zip(starts, starts[1:])]
    dones = [allring[k][3] / 1000 for k in ks if allring[k][3]]
    acqs = [allring[k][4] / 1000 for k in ks if allring[k][4]]
    # GPU service time: completion seen minus max(submit end, previous completion); busy = their sum over the window
    svc, prev = [], 0
    for k in ks:
        s0, sl, _p, dn, _a = allring[k]
        if not dn:
            continue
        end = s0 + sl; done = end + dn
        svc.append((done - max(end, prev)) / 1000); prev = max(prev, done)
    frames = b['done'] - a0['done']
    sub = b['n'] - a0['n']
    dn = max(sub, 1)
    # Motion fps: the window also holds the time after the animation ends, when the render server presents only what changes (the
    # home screen's idle clock, 10 Hz), so frames / window understates the rate while something moves. The motion run is the longest
    # stretch of submissions under 50 ms apart; its frames are its submissions over the window's submissions per frame.
    run, best, first = 0.0, (0.0, 0), 0
    for i, g in enumerate(gaps):
        if g < 50:
            run += g
            if run > best[0]: best = (run, i + 1 - first)
        else:
            run, first = 0.0, i + 1
    per = sub / frames if frames else 0
    motion = (best[1] / per) / (best[0] / 1000) if per and best[0] > 0 else 0
    res = dict(spec=spec, win=win, frames=frames, fps=frames / win if win else 0, motion=motion, motion_s=best[0] / 1000, sub=sub, missing=sub - len(ks),
               tot_avg=st.mean(tot) / 1000 if tot else 0, tot_max=max(tot) / 1000 if tot else 0,
               pub_avg=st.mean(pub) / 1000 if pub else 0, imp_kib=(b['imp'] - a0['imp']) / dn / 1024,
               own_kib=(b['own'] - a0['own']) / dn / 1024, unc_kib=(b['unc'] - a0['unc']) / dn / 1024,
               gap_med=st.median(gaps) if gaps else 0, gap_max=max(gaps) if gaps else 0,
               retire=(b['retire'] - a0['retire']) / dn / 1000, acq=b['acq'] - a0['acq'], acqskip=b['acqskip'] - a0['acqskip'],
               done_med=st.median(dones) if dones else 0, done_max=max(dones) if dones else 0, acq_avg=st.mean(acqs) if acqs else 0,
               svc_med=st.median(svc) if svc else 0, busy=sum(svc) / 1000 / win * 100 if win else 0)
    print('{spec:>8}: {frames} frames in {win:.2f} s = {fps:.1f} fps | motion {motion:.1f} fps over {motion_s:.2f} s | {sub} submits (ring miss {missing}) total avg {tot_avg:.1f} '
          'max {tot_max:.1f} ms publish avg {pub_avg:.1f} ms | per submit imp {imp_kib:.0f} own {own_kib:.0f} unc-skip {unc_kib:.0f} KiB'
          ' | submit gap med {gap_med:.1f} max {gap_max:.1f} ms | retire {retire:.1f} ms acquires {acq} skipped {acqskip} avg {acq_avg:.1f} ms | complete after med {done_med:.1f} max {done_max:.1f} ms | gpu service med {svc_med:.1f} ms busy {busy:.0f}%'.format(**res), flush=True)
    return res


def main():
    specs = sys.argv[1:] or ['left', 'right', 'left', 'right', 'up']
    rs = [r for r in (one(s) for s in specs) if r]
    if rs:
        print('MEAN: {:.1f} fps (motion {:.1f}), submit {:.1f} ms, publish {:.1f} ms, gap med {:.1f} ms'.format(
            st.mean(r['fps'] for r in rs), st.mean(r['motion'] for r in rs), st.mean(r['tot_avg'] for r in rs), st.mean(r['pub_avg'] for r in rs),
            st.mean(r['gap_med'] for r in rs)))


main()
