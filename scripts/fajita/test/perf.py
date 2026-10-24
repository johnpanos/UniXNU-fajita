#!/usr/bin/env python3
"""perf.py [SWIPE [MS]]: fire a swipe (default left:100) and read the a6xx counters over MS (kcmd gpu perf); print ratios."""
import os
import subprocess, sys, re
HUB = ['python3', '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'fajita-usbdbg-hub.py'), 'call', '--timeout', '12']
sw = sys.argv[1] if len(sys.argv) > 1 else 'left:100'; ms = sys.argv[2] if len(sys.argv) > 2 else '800'
d, _, f = sw.partition(':')
if d != 'none':
    subprocess.run(HUB + ['kcmd', 'touch', 'swipe', d] + ([f] if f else []), capture_output=True)
out = subprocess.run(HUB + ['kcmd', 'gpu', 'perf', ms], capture_output=True, text=True).stdout
v = {m.group(1): int(m.group(2)) for m in re.finditer(r'^(\w+) (\d+)$', out, re.M)}
win = int(re.search(r'window (\d+)', out).group(1))
g = lambda k: v.get(k, 0)
pct = lambda a, b: 100.0 * a / b if b else 0
print(f"window {win/1000:.0f} ms | gpu clock {g('rbbm_always')/win:.0f} MHz | gpu busy {pct(g('gpu_busy'), g('rbbm_always')):.0f}% | "
      f"cp busy {pct(g('cp_busy'), g('rbbm_always')):.0f}% (core idle while cp busy {pct(g('cp_busy_core_idle'), g('cp_busy')):.0f}%)")
print(f"sp busy {pct(g('sp_busy'), g('rbbm_always')):.0f}% alu {pct(g('sp_alu'), g('sp_busy')):.0f}% of busy, stall on tp {pct(g('sp_stall_tp'), g('sp_busy')):.0f}%, wave idle {g('sp_wave_idle')}")
print(f"tp busy {pct(g('tp_busy'), g('rbbm_always')):.0f}% stall on uche {pct(g('tp_stall_uche'), g('tp_busy')):.0f}% | l1 req {g('tp_l1_req')} miss {pct(g('tp_l1_miss'), g('tp_l1_req')):.0f}%")
print(f"uche busy {pct(g('uche_busy'), g('rbbm_always')):.0f}% arb stall {pct(g('uche_stall_arb'), g('uche_busy')):.0f}% | vbif latency {g('uche_vbif_lat_cyc')/max(g('uche_vbif_lat_n'),1):.0f} cycles avg over {g('uche_vbif_lat_n')} samples")
print(out.strip().replace('\n', ' '))
