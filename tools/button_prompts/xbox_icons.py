"""Redraws the PS4 button icons of an Uncharted symbol3 font atlas as Xbox ones (mock-up).

usage: xbox_icons.py ATLAS.tga [ALPHA.tga] FONT.fnt OUT.png
"""
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONT = r'C:\Users\rgegg\github\uncharted_pc\launcher\fonts\Cinzel-Bold.ttf'
SANS = r'C:\Users\rgegg\github\uncharted_pc\gpu\third_party\fonts\DejaVuSans.ttf'

# Face buttons: PS4 symbol (by position in the atlas) -> Xbox letter and colour.
FACE = {'circle': ('B', (226, 55, 44)), 'square': ('X', (40, 125, 230)),
        'triangle': ('Y', (240, 190, 40)), 'cross': ('A', (100, 190, 60))}
# Labelled shapes: old label -> new label.
RELABEL = {'R2': 'RT', 'L2': 'LT', 'R1': 'RB', 'L1': 'LB', 'L3': 'LS', 'R3': 'RS',
           'OPTIONS': 'MENU'}


def load(path, alpha_path=None):
    im = Image.open(path).convert('RGBA')
    if alpha_path:
        im.putalpha(Image.open(alpha_path).convert('L'))
    return im


def chars(fnt):
    out = {}
    for line in open(fnt, encoding='latin-1'):
        if line.startswith('char '):
            v = dict(re.findall(r'(\w+)=(-?\d+)', line))
            out[int(v['id'])] = tuple(int(v[k]) for k in ('x', 'y', 'width', 'height'))
    return out


def face_button(size, letter, colour):
    """A dark round button with a coloured letter and the atlas's coloured glow."""
    s = 4  # supersample
    w = size * s
    im = Image.new('RGBA', (w, w), (0, 0, 0, 0))
    c = w / 2
    r = w * 0.36
    glow = Image.new('RGBA', (w, w), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((c - r * 1.12, c - r * 1.12, c + r * 1.12, c + r * 1.12),
                                 fill=colour + (230,))
    glow = glow.filter(ImageFilter.GaussianBlur(w * 0.05))
    im.alpha_composite(glow)
    d = ImageDraw.Draw(im)
    # Rim, then the face with a soft top-to-bottom shade.
    d.ellipse((c - r, c - r, c + r, c + r), fill=(150, 140, 125, 255))
    face = Image.new('RGBA', (w, w), (0, 0, 0, 0))
    shade = Image.linear_gradient('L').resize((w, w))
    top, bottom = (58, 54, 50), (18, 17, 16)
    body = Image.composite(Image.new('RGB', (w, w), bottom), Image.new('RGB', (w, w), top), shade)
    mask = Image.new('L', (w, w), 0)
    ri = r * 0.9
    ImageDraw.Draw(mask).ellipse((c - ri, c - ri, c + ri, c + ri), fill=255)
    face.paste(body, (0, 0), mask)
    im.alpha_composite(face)
    font = ImageFont.truetype(SANS, int(r * 1.15))
    d = ImageDraw.Draw(im)
    d.text((c, c + r * 0.02), letter, font=font, fill=colour + (255,), anchor='mm')
    return im.resize((size, size), Image.LANCZOS)


def erase_label(cell):
    """Removes the light label from a dark button shape: light pixels are filled in from
    their dark surroundings."""
    rgb = cell.convert('RGB')
    lum = rgb.convert('L')
    w, h = cell.size
    box = Image.new('L', cell.size, 0)
    ImageDraw.Draw(box).rectangle((w * 0.15, h * 0.12, w * 0.85, h * 0.92), fill=255)
    opaque = cell.getchannel('A').point(lambda v: 255 if v > 160 else 0)
    text = ImageChops.multiply(lum.point(lambda v: 255 if v > 110 else 0), box)
    text = ImageChops.multiply(text, opaque)
    text = text.filter(ImageFilter.MaxFilter(5))
    out = cell.copy()
    alpha = cell.getchannel('A')
    out.paste(fill_dark(rgb, text), (0, 0), text)
    out.putalpha(alpha)
    return out


def fill_dark(rgb, mask):
    """Average colour of the non-masked dark pixels, as a flat fill with the shape's shade."""
    import statistics
    px = rgb.load()
    m = mask.load()
    w, h = rgb.size
    rows = []
    for y in range(h):
        vals = [px[x, y] for x in range(w) if not m[x, y] and sum(px[x, y]) < 330 and sum(px[x, y]) > 30]
        rows.append(tuple(int(statistics.median(c)) for c in zip(*vals)) if vals else None)
    last = (30, 28, 26)
    flat = Image.new('RGB', rgb.size)
    for y in range(h):
        if rows[y]:
            last = rows[y]
        ImageDraw.Draw(flat).line((0, y, w, y), fill=last)
    return flat


def label(cell, text, small=False):
    out = erase_label(cell)
    w, h = cell.size
    size = int(h * (0.36 if len(text) <= 2 else 0.24))
    font = ImageFont.truetype(FONT, size)
    d = ImageDraw.Draw(out)
    d.text((w / 2, h * 0.55), text, font=font, fill=(235, 225, 205, 255), anchor='mm')
    return out


def caption(cell, old_h, text):
    """OPTIONS/SHARE style: the caption over the small pill, cream with a dark outline."""
    out = cell.copy()
    w, h = cell.size
    out.paste(Image.new('RGBA', (w, old_h), (0, 0, 0, 0)), (0, 0))
    size = int(old_h * 0.95)
    font = ImageFont.truetype(FONT, size)
    while size > 6 and font.getlength(text) > w - 4:
        size -= 1
        font = ImageFont.truetype(FONT, size)
    d = ImageDraw.Draw(out)
    d.text((w / 2, old_h * 0.55), text, font=font, fill=(250, 240, 200, 255), anchor='mm',
           stroke_width=max(1, old_h // 10), stroke_fill=(10, 10, 10, 255))
    return out


def view_button(cell):
    """The touchpad button with the Xbox View symbol (two overlapping windows) for the hand."""
    out = erase_label(cell)
    w, h = cell.size
    s = 4
    big = Image.new('RGBA', (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    u = h * s * 0.2
    cx, cy = w * s / 2, h * s / 2
    lw = int(u * 0.28)
    col = (235, 225, 205, 255)
    d.rectangle((cx - u * 1.1, cy - u * 0.9, cx + u * 0.5, cy + u * 0.3), outline=col, width=lw)
    d.rectangle((cx - u * 0.5, cy - u * 0.3, cx + u * 1.1, cy + u * 0.9), fill=(30, 28, 26, 255),
                outline=col, width=lw)
    out.alpha_composite(big.resize((w, h), Image.LANCZOS))
    return out


def opaque_top(cell):
    """Rows from the top until the pill starts: the caption's height."""
    a = cell.getchannel('A').load()
    w, h = cell.size
    # the pill is the lowest opaque run; the caption ends where a fully clear row follows it
    rows = [any(a[x, y] > 100 for x in range(w)) for y in range(h)]
    y = 0
    while y < h and not rows[y]:
        y += 1
    while y < h and rows[y]:
        y += 1
    return y


def main():
    args = sys.argv[1:]
    atlas_path, rest = args[0], args[1:]
    alpha = rest.pop(0) if rest[0].endswith('.tga') else None
    fnt, out_path = rest
    im = load(atlas_path, alpha)
    ch = chars(fnt)
    # Ids by what they show (the same layout in all three games).
    faces = {2: 'circle', 3: 'square', 4: 'triangle', 5: 'cross'}
    labels = {7: 'RT', 9: 'LT', 6: 'RB', 8: 'LB', 24: 'LS', 25: 'RS',
              41: 'RT', 43: 'LT', 40: 'RB', 42: 'LB', 47: 'LS', 48: 'RS'}
    for i, name in faces.items():
        x, y, w, h = ch[i]
        letter, colour = FACE[name]
        im.paste(Image.new('RGBA', (w, h), (0, 0, 0, 0)), (x, y))
        im.alpha_composite(face_button(w, letter, colour), (x, y))
    for i, text in labels.items():
        x, y, w, h = ch[i]
        im.paste(label(im.crop((x, y, x + w, y + h)), text), (x, y))
    for i, text in {11: 'MENU', 44: 'MENU'}.items():
        x, y, w, h = ch[i]
        cell = im.crop((x, y, x + w, y + h))
        im.paste(caption(cell, int(h * 0.36), text), (x, y))
    x, y, w, h = ch[14]
    im.paste(view_button(im.crop((x, y, x + w, y + h))), (x, y))
    im.save(out_path)


if __name__ == '__main__':
    main()
