#!/usr/bin/env python3
"""find-icon.py SHOT.png [TEMPLATE.png]: where an app icon (default: $FAJITA_WORK/settings-icon.png, 80x80 from a half-resolution screenshot)
is on a half-resolution home-screen screenshot. Prints 'PX PY SCORE' with PX,PY in panel coordinates (x2) for kcmd touch tap; SCORE is the
mean absolute difference per channel (a real match is < 25: the corners show the wallpaper). Exit 1 when nothing matches."""
import os
import sys, numpy as np
from PIL import Image
shot = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
tpl = np.asarray(Image.open(sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.environ.get('FAJITA_WORK') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '.cache', 'fajita-work'), 'settings-icon.png')).convert('RGB'), dtype=np.int16)
th, tw = tpl.shape[:2]; H, W = shot.shape[:2]
best = (1e9, 0, 0)
for coarse in (4,):
    for y in range(60, min(H - th, 1000), coarse):
        rows = shot[y:y + th]
        for x in range(0, W - tw, coarse):
            d = np.abs(rows[:, x:x + tw] - tpl).mean()
            if d < best[0]: best = (d, x, y)
d, bx, by = best
for y in range(max(0, by - 4), min(H - th, by + 5)):
    for x in range(max(0, bx - 4), min(W - tw, bx + 5)):
        e = np.abs(shot[y:y + th, x:x + tw] - tpl).mean()
        if e < d: d, bx, by = e, x, y
print('%d %d %.1f' % ((bx + tw // 2) * 2, (by + th // 2) * 2, d))
sys.exit(0 if d < 25 else 1)
