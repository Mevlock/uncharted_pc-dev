"""mask_sheet.py <candidate dir> <out prefix>: contact sheets of the outlier masks of isolated candidates.

Each tile is the middle frame (dimmed) with outlier pixels (differs from both neighbours by > 40
while they agree within 15) in white, labelled with the frame number. Thin long white lines = spikes."""
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

d, prefix = sys.argv[1], sys.argv[2]
fs = sorted(os.listdir(d))
nums = [int(re.match(r'glitch_(\d+)_', f).group(1)) for f in fs]
s = set(nums)
iso = [(n, f) for n, f in zip(nums, fs) if n - 1 not in s and n + 1 not in s]
tw, th = 430, 180
cols, rows = 4, 6
tiles = []
for n, f in iso:
    im = np.asarray(Image.open(os.path.join(d, f)).convert('L')).astype(np.int16)
    w = im.shape[1] // 3
    p, c, x = im[:, :w], im[:, w:2 * w], im[:, 2 * w:3 * w]
    m = (np.abs(c - p) > 40) & (np.abs(c - x) > 40) & (np.abs(p - x) < 15)
    out = (c // 4).astype(np.uint8)
    out[m] = 255
    t = Image.fromarray(out).resize((tw, th), Image.BOX)
    ImageDraw.Draw(t).text((4, 4), str(n), fill=255)
    tiles.append(t)
per = cols * rows
for i in range(0, len(tiles), per):
    sheet = Image.new('L', (cols * tw, rows * th))
    for j, t in enumerate(tiles[i:i + per]):
        sheet.paste(t, ((j % cols) * tw, (j // cols) * th))
    sheet.save(f'{prefix}_{i // per}.png')
print(len(tiles), 'tiles')
