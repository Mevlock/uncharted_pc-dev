"""oneframe_scan.py <video> <out dir>: finds one-frame glitches (geometry spikes, HUD flicker).

A pixel is an outlier when the frame differs from both neighbours by > 40 grey levels while the
neighbours agree within 15. Frames with > THRESHOLD outlier pixels (at 1720x720, grey) are
candidates; each is saved as prev | frame | next. Duplicate frames (recording faster than the
game) are dropped. Fast motion also produces candidates: review them with mask_sheet.py, and the
largest ones (real spikes in U3's pub were 36k-340k pixels) by eye. FFMPEG overrides the path."""
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

THRESHOLD = 8000
FFMPEG = os.environ.get("FFMPEG", r"C:\msys64\clang64\bin\ffmpeg.exe")
video, out = sys.argv[1], Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
w, h = 1720, 720
cmd = [FFMPEG, '-v', 'error', '-i', video, '-vf',
       f'mpdecimate,scale={w}:{h}:flags=area', '-fps_mode', 'vfr', '-f', 'rawvideo',
       '-pix_fmt', 'gray', '-']
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
window = []
index, hits = 0, []
while True:
    raw = proc.stdout.read(w * h)
    if len(raw) < w * h:
        break
    rgb = np.frombuffer(raw, np.uint8).reshape(h, w)
    window.append(rgb)
    if len(window) > 3:
        window.pop(0)
    index += 1
    if len(window) < 3:
        continue
    p, c, n = (x.astype(np.int16) for x in window)
    d1, d2, d3 = np.abs(c - p), np.abs(c - n), np.abs(p - n)
    count = int(((d1 > 40) & (d2 > 40) & (d3 < 15)).sum())
    if count > THRESHOLD:
        frame = index - 2
        hits.append((frame, count))
        Image.fromarray(np.concatenate(window, axis=1)).save(out / f'glitch_{frame:06d}_{count}.png')
proc.wait()
print(f'{index} distinct frames, {len(hits)} one-frame glitches '
      f'({len(hits) / max(index, 1) * 1000:.2f} per 1000 frames)')
for frame, count in hits:
    print(f'  frame {frame}: {count} pixels')
