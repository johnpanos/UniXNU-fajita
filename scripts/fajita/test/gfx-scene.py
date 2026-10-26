#!/usr/bin/env python3
"""gfx-scene.py OUT.json [--cpu NAME,...] [--settle S] [--tool PATH] [--cawatch FJSURFACE] STEP...: run one scene of the gfx suite on the held session and
record everything the analyzer (tools/gpu/gfx-report.py) needs, in one JSON file.

Steps (run in order; touch coordinates are panel pixels, 2x the half-resolution screenshot's):
  left|right|up|down[:FRAMES]        an on-demand swipe (kcmd touch swipe DIR [FRAMES]); after it ends, SETTLE s (default 1.0)
  xy,X0,Y0,X1,Y1[:FRAMES]            a drag between two points (a list scroll; FRAMES 60 is a slow drag with no fling)
  tap,X,Y                            a tap (an app icon, a button)
  wait,S                             S seconds of nothing (idle: not a motion window)
  shot,NAME                          a screenshot at rest: OUT-NAME.png beside OUT.json (the correctness rest point)
  open,URL                           open URL as SpringBoard does (fjbundle --open; --fjbundle PATH): a launch, measured like a tap
  anim,S                             S seconds of a page's own animation (no touch): measured like a finger on the screen
Every touch step is a motion window for the analyzer: from the gesture's start (the kernel's touch clock) to the end of the motion
it caused (frames keep coming after the lift: a fling, a page snap), see gfx-report.py.

Recorded (all times in us of the mach clock, the kernel log's):
  latch    every frame the panel started showing (FajitaDpu.cpp fajita_dpu_latch_ring: the VIG0 flip latch, fajita-fb-flip)
  retire   every swap the display retired (LegacyDisplayProbe.cpp native_legacy_retire_ring)
  vsync    every PP0 rd_ptr (the panel's TE-paced frame boundary)
  gov      the GPU governor's 20 ms samples (FajitaGpuProbe.cpp fajita_gpu_gov_ring): busy and always-on cycles, level, cap, tsens
  perf     the a6xx counters over the scene (kcmd gpu perf begin / end)
  cpu      `ngpu-metal cpu` windows back to back over the scene (--cpu: the processes whose threads are shown; --tool: the binary,
           default /private/var/ngpu/sbin/ngpu-metal): per core, per process, per thread with run/wait shares
  counters display and hand-off counters before and after (offers, completions, reclaimed, retire_lost, flips), kcmd gpu status
  ca       (--cawatch, instead of cpu: the userland agent runs one command at a time) the render server's display-timer state,
           `fjsurface --watch-ca` windows back to back: each change of its last tick, vbl threshold, next update, idle countdown,
           can_update failures, swap ring and timer deadline (raw lines; tools/gpu/ca-timer.py classifies every tick)
A ring that wrapped during the scene (more entries than it holds) is reported in the JSON's "overflow" and on stderr, never dropped
silently. Env: KM (the running kernel's kernel.macho for the symbols; default the build output), FAJITA_USBDBG_HUB (hub socket)."""
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BOARD = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
SOCK = os.environ.get('FAJITA_USBDBG_HUB', os.path.join(os.environ.get('XDG_RUNTIME_DIR', '/tmp'), 'fjdbg-hub.sock'))
KM = os.environ.get('KM') or os.path.join(os.environ.get('FAJITA_OUT') or os.path.join(BOARD, '.cache', 'fajita-out'),
                                          'unixnu', 'fajita-ios16', 'kernel', 'kernel.macho')
NS_PER_TICK = 625 / 12   # the 19.2 MHz mach clock (52.083 ns a tick)
fjbundle = '/private/var/ngpu/sbin/fjbundle'


def hub(op, *args, timeout=10):
    """One request through the hub (scripts/fajita-usbdbg-hub.py's call, without a process per call)."""
    try:
        with open(SOCK + '.activity', 'a'):
            os.utime(SOCK + '.activity')
    except OSError:
        pass
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout + 15)
    s.connect(SOCK)
    f = s.makefile('rwb')
    f.write((json.dumps({'op': op, 'args': [str(a) for a in args], 'timeout': timeout}) + '\n').encode())
    f.flush()
    rep = json.loads(f.readline())
    s.close()
    return rep['ok'], rep['out']


_syms = {}


def sym(name):
    if not _syms:
        out = subprocess.run(['llvm-nm', KM], capture_output=True, text=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3:
                _syms.setdefault(p[2].lstrip('_'), int(p[0], 16))
        if not _syms:
            sys.exit('gfx-scene: no symbols from %s (KM)' % KM)
    return _syms.get(name)


def readv(addr, n):
    data = b''
    while len(data) < n:
        k = min(1024, n - len(data))
        for attempt in range(3):
            ok, out = hub('readv', hex(addr + len(data)), k)
            if ok:
                break
            time.sleep(0.2)
        else:
            raise RuntimeError('readv %#x: %s' % (addr + len(data), out.strip()[:80]))
        data += bytes.fromhex(out.strip())
    return data


def ring(name, n0, n1, size, entry, cap, overflow):
    """Entries n0..n1-1 of a ring of `size` entries of `entry` bytes; a wrap (more than the ring holds) keeps the newest."""
    if n1 - n0 > size:
        overflow[name] = n1 - n0 - size
        print('gfx-scene: !! %s wrapped: %d entries lost (the scene is longer than the ring)' % (name, n1 - n0 - size), file=sys.stderr)
        n0 = n1 - size
    if n1 <= n0:
        return []
    base = sym(name)
    out = b''
    k = n0
    while k < n1:   # contiguous pieces of the ring
        i = k % size
        m = min(n1 - k, size - i)
        out += readv(base + i * entry, m * entry)
        k += m
    return [out[j * entry:(j + 1) * entry] for j in range(n1 - n0)]


# (key, symbol, index, bytes)
COUNTERS = [('latch_n', 'fajita_dpu_latch_n', 0, 4), ('vsync_n', 'fajita_dpu_vsync_n', 0, 4),
            ('retire_n', 'native_legacy_retire_ring_n', 0, 4), ('gov_n', 'fajita_gpu_gov_n', 0, 4),
            ('offers', 'native_gop_gpu_offers', 0, 8), ('completions', 'native_gop_gpu_completions', 0, 8),
            ('failures', 'native_gop_gpu_failures', 0, 8), ('abandoned', 'native_gop_gpu_abandoned', 0, 8),
            ('reclaimed', 'native_gop_gpu_reclaimed', 0, 8), ('retire_lost', 'native_legacy_retire_lost', 0, 4),
            ('flips_requested', 'fajita_dpu_flips', 0, 4), ('flips_latched', 'fajita_dpu_flips', 1, 4),
            ('flips_refused', 'fajita_dpu_flips', 2, 4), ('scanned_writes', 'fajita_dpu_flips', 3, 4),
            ('gpu_fallbacks', 'native_legacy_gpu_fallbacks', 0, 4), ('copies', 'native_legacy_copy_n', 0, 8),
            ('slot_busy', 'native_adreno_submit_backpressure', 0, 4), ('ring_full', 'native_adreno_submit_backpressure', 1, 4),
            ('peak_jobs', 'native_adreno_submit_backpressure', 2, 4),
            ('fi_sent', 'native_legacy_frame_info', 0, 4), ('fi_clients', 'native_legacy_frame_info', 1, 4),
            ('fi_failed', 'native_legacy_frame_info', 2, 4), ('fi_noclient', 'native_legacy_frame_info', 3, 4),
            ('fi_id_differs', 'native_legacy_frame_info', 4, 4), ('fi_last_id', 'native_legacy_frame_info', 5, 4),
            ('fi_leaf292', 'native_legacy_frame_info_leaf', 0, 4), ('fi_leaf211', 'native_legacy_frame_info_leaf', 1, 4)]


def counters():
    c = {}
    for key, name, idx, size in COUNTERS:
        a = sym(name)
        try:
            c[key] = None if a is None else int.from_bytes(readv(a + size * idx, size), 'little')
        except RuntimeError as e:
            c[key] = None
            print('gfx-scene: %s: %s' % (key, e), file=sys.stderr)
    ok, out = hub('kcmd', 'gpu', timeout=10)
    c['gpu_status'] = out.strip()
    m = re.search(r'seen ([0-9a-f]+) first-error ([0-9a-f]+) hwfault ([0-9a-f]+)', out)
    if m:
        c['irq_seen'], c['irq_first_error'], c['irq_hwfault'] = (int(x, 16) for x in m.groups())
    m = re.search(r'iofaults (\d+)', out)
    if m:
        c['iofaults'] = int(m.group(1))
    return c


def touch_status():
    ok, out = hub('kcmd', 'touch', timeout=10)
    m = re.search(r'now (\d+) ms req (\d+) taken (\d+) done (\d+) start (\d+) end (\d+)', out)
    return dict(zip(('now', 'req', 'taken', 'done', 'start', 'end'), map(int, m.groups()))) if m else None


def gesture(spec):
    """Fire a touch step; return its record once it ended (kernel touch clock, ms)."""
    d, _, frames = spec.partition(':')
    if d.startswith('xy,'):
        args = ['swipe', 'xy'] + d.split(',')[1:] + ([frames] if frames else [])
    elif d.startswith('tap,'):
        args = ['tap'] + d.split(',')[1:]
    else:
        args = ['swipe', d] + ([frames] if frames else [])
    for attempt in range(20):
        ok, q = hub('kcmd', 'touch', *args, timeout=10)
        m = re.search(r'queued (\d+)', q)
        if m:
            break
        if 'busy' not in q:
            return dict(spec=spec, error=q.strip()[:120])
        time.sleep(0.1)
    else:
        return dict(spec=spec, error='touch busy')
    gid = int(m.group(1))
    limit = time.time() + 30
    while time.time() < limit:
        t = touch_status()
        if t and t['done'] >= gid:
            return dict(spec=spec, kind='touch', id=gid, start_us=t['start'] * 1000, end_us=t['end'] * 1000)
        time.sleep(0.05)
    return dict(spec=spec, error='gesture %d did not finish' % gid)


def now_us():
    t = touch_status()
    return t['now'] * 1000 if t else None


CPU_LINE = re.compile(r'^\s*([\d.]+)\s+([\d.]+)%\s+(\d+)\s+(.*)$')
THREAD_LINE = re.compile(r'^\s*([\d.]+)\s+([\d.]+)%\s+(0x[0-9a-f]+)\s+(.*?) \| run (\d+)% wait (\d+)% pri (-?\d+)$')


def parse_cpu(text):
    w = dict(procs=[], threads={}, cores=[])
    cur = None
    for line in text.splitlines():
        m = re.match(r'cpu over ([\d.]+) ms from ([\d.]+) us', line)
        if m:
            w['ms'], w['start_us'] = float(m.group(1)), float(m.group(2))
            continue
        if line.startswith('cores:'):
            w['cores'] = [tuple(map(float, x.split(':')[1].split('/'))) for x in line.split()[1:] if ':' in x and '/' in x]
            continue
        m = re.match(r'threads of (\d+)', line)
        if m:
            cur = m.group(1)
            w['threads'][cur] = []
            continue
        m = THREAD_LINE.match(line)
        if m and cur:
            w['threads'][cur].append(dict(ms=float(m.group(1)), tid=m.group(3), name=m.group(4), run=int(m.group(5)),
                                          wait=int(m.group(6)), pri=int(m.group(7))))
            continue
        m = CPU_LINE.match(line)
        if m and cur is None:
            w['procs'].append(dict(ms=float(m.group(1)), pid=int(m.group(3)), name=m.group(4).strip()))
    return w


class CpuSampler(threading.Thread):
    """`ngpu-metal cpu WINDOW NAME...` back to back until stopped (the userland agent runs one command at a time; nothing else in the
    scene uses it)."""

    def __init__(self, tool, names, window=2.0):
        super().__init__(daemon=True)
        self.tool, self.names, self.window, self.stop, self.windows, self.errors = tool, names, window, False, [], []

    def run(self):
        while not self.stop:
            ok, out = hub('run', self.tool, 'cpu', '%.1f' % self.window, *self.names, timeout=int(self.window) + 20)
            w = parse_cpu(out)
            if 'ms' in w:
                self.windows.append(w)
            else:
                self.errors.append(out.strip()[:200])
                time.sleep(0.5)


class CaWatcher(threading.Thread):
    """`fjsurface --watch-ca PID WINDOW` back to back until stopped; the first window finds the server and timer, later ones reuse them."""

    def __init__(self, tool, window=3.0):
        super().__init__(daemon=True)
        self.tool, self.window, self.stop, self.lines, self.errors, self.server, self.timer = tool, window, False, [], [], '', ''

    def run(self):
        ok, ps = hub('run', '/usr/sbin/ngpu-metal', 'ps', timeout=15)
        pid = next((l.split()[0] for l in ps.splitlines() if len(l.split()) > 1 and l.split()[1] == 'backboardd'), None)
        if not pid:
            self.errors.append('no backboardd in ps')
            return
        while not self.stop:
            args = [self.tool, '--watch-ca', pid, '%.1f' % self.window, '200'] + ([self.server] if self.server else []) + \
                   ([self.timer] if self.server and self.timer else [])
            ok, out = hub('run', *args, timeout=int(self.window) + 30)
            for l in out.splitlines():
                m = re.match(r'server (0x[0-9a-f]+)', l)
                if m:
                    self.server = m.group(1)
                m = re.match(r'timer (0x[0-9a-f]+)', l)
                if m:
                    self.timer = m.group(1)
                if l.startswith('t '):
                    self.lines.append(l)
            if not self.server:
                self.errors.append(out.strip()[:200])
                return


def main():
    a = sys.argv[1:]
    if not a or a[0] in ('-h', '--help'):
        print(__doc__.strip())
        return 2
    out = a.pop(0)
    global fjbundle
    settle, cpu_names, tool, cawatch = 1.0, ['backboardd'], '/private/var/ngpu/sbin/ngpu-metal', None
    while a and a[0].startswith('--'):
        o = a.pop(0)
        if o == '--settle':
            settle = float(a.pop(0))
        elif o == '--cpu':
            cpu_names = [x for x in a.pop(0).split(',') if x]
        elif o == '--tool':
            tool = a.pop(0)
        elif o == '--cawatch':
            cawatch = a.pop(0)
        elif o == '--fjbundle':
            fjbundle = a.pop(0)
        else:
            sys.exit('gfx-scene: unknown option %s' % o)
    scene = os.path.splitext(os.path.basename(out))[0]
    rec = dict(scene=scene, km=KM, steps=[], overflow={}, settle=settle, cpu_names=cpu_names)
    c0 = counters()
    ok, p = hub('kcmd', 'gpu', 'perf', 'begin', timeout=10)
    rec['perf_begin'] = p.strip()
    sampler = CaWatcher(cawatch) if cawatch else CpuSampler(tool, cpu_names) if tool != 'none' else None
    if sampler:
        sampler.start()
        time.sleep(0.3)   # the first window is open before the first gesture
    rec['t0_us'] = now_us()
    for spec in a:
        if spec.startswith('wait,'):
            t0 = now_us()
            time.sleep(float(spec.split(',')[1]))
            rec['steps'].append(dict(spec=spec, kind='wait', start_us=t0, end_us=now_us()))
            continue
        if spec.startswith('open,'):
            # open a URL the way SpringBoard does (fjbundle --open): an app launch (Safari for http(s), an app's scheme). The motion window
            # starts when the open returns, like a tap's lift; the userland agent may first finish a CPU window (at most ~2 s).
            t0 = now_us()
            ok, r = hub('run', fjbundle, '--open', spec.split(',', 1)[1], timeout=30)
            t1 = now_us()
            rec['steps'].append(dict(spec='tap-' + spec, kind='touch', start_us=t1, end_us=t1, issued_us=t0, out=r.strip()[-200:]))
            time.sleep(settle)
            continue
        if spec.startswith('anim,'):
            # S seconds of animation without touch (a page's own animation, a video): every vsync without a new frame is a drop
            t0 = now_us()
            time.sleep(float(spec.split(',')[1]))
            rec['steps'].append(dict(spec=spec, kind='touch', start_us=t0, end_us=now_us()))
            continue
        if spec.startswith('shot,'):
            path = os.path.join(os.path.dirname(os.path.abspath(out)), '%s-%s.png' % (scene, spec.split(',', 1)[1]))
            t0 = now_us()
            ok, r = hub('screenshot', path, timeout=40)
            rec['steps'].append(dict(spec=spec, kind='shot', start_us=t0, end_us=now_us(), path=path, ok=ok))
            continue
        g = gesture(spec)
        rec['steps'].append(g)
        if 'error' in g:
            print('gfx-scene: %s: !! %s' % (spec, g['error']), file=sys.stderr)
        time.sleep(settle)
    rec['t1_us'] = now_us()
    if sampler:
        sampler.stop = True
    ok, p = hub('kcmd', 'gpu', 'perf', 'end', timeout=10)
    rec['perf'] = {m.group(1): int(m.group(2)) for m in re.finditer(r'^(\w+) (\d+)$', p, re.M)}
    m = re.search(r'window (\d+)', p)
    rec['perf_window_us'] = int(m.group(1)) if m else None
    c1 = counters()
    rec['counters0'], rec['counters1'] = c0, c1
    ov = rec['overflow']
    tick = lambda b: struct.unpack('<Q', b)[0] * NS_PER_TICK / 1000
    if c0.get('latch_n') is not None and c1.get('latch_n') is not None:
        rec['latch'] = [tick(e) for e in ring('fajita_dpu_latch_ring', c0['latch_n'], c1['latch_n'], 2048, 8, 0, ov)]
    if c0.get('vsync_n') is not None and c1.get('vsync_n') is not None:
        rec['vsync'] = [tick(e) for e in ring('fajita_dpu_vsync_ring', max(c0['vsync_n'], c1['vsync_n'] - 2048), c1['vsync_n'], 2048, 8, 0, {})]
    if c0.get('retire_n') is not None and c1.get('retire_n') is not None:
        rec['retire'] = [tick(e) for e in ring('native_legacy_retire_ring', c0['retire_n'], c1['retire_n'], 2048, 8, 0, ov)]
    if c0.get('gov_n') is not None and c1.get('gov_n') is not None:
        rec['gov'] = [dict(zip(('us', 'always', 'busy', 'level', 'cap', 'temp', 'bus'), struct.unpack('<QIIHHhH', e)))
                      for e in ring('fajita_gpu_gov_ring', c0['gov_n'], c1['gov_n'], 4096, 24, 0, ov)]
    a_cf = sym('fajita_cpufreq_rows')
    if a_cf:
        rec['cpufreq_rows'] = [readv(a_cf + 256 * i, 256).split(b'\0')[0].decode(errors='replace') for i in range(6)]
    ok, g = hub('kcmd', 'gpu', 'gov', timeout=10)
    rec['gov_status'] = g.strip()
    if sampler:
        sampler.join(timeout=40)
        if cawatch:
            rec['ca'], rec['ca_errors'], rec['ca_server'] = sampler.lines, sampler.errors, sampler.server
        else:
            rec['cpu'] = sampler.windows
            rec['cpu_errors'] = sampler.errors
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    json.dump(rec, open(out, 'w'))
    lat = rec.get('latch', [])
    bad = [s['spec'] for s in rec['steps'] if 'error' in s]
    print('%s: %d steps%s, %d frames latched, %d retired, %d gov samples, %d cpu windows%s' % (
        scene, len(rec['steps']), ' (%d FAILED: %s)' % (len(bad), ' '.join(bad)) if bad else '', len(lat), len(rec.get('retire', [])),
        len(rec.get('gov', [])), len(rec.get('cpu', [])), ' | overflow %s' % ov if ov else ''))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
