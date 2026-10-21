"""Draws the Xbox button pictures in a domed style: dark rounded buttons with coloured letters and a
soft glow for A/B/X/Y, the same dark finish for the bumpers, triggers, sticks, d-pad, Menu and View.
Closer to the games' own button art than flat icons. Drawn here from shapes and Liberation Sans
Bold (SIL OFL 1.1, tools/fonts), nothing taken from the games.

draw(name) returns one 128x128 RGBA picture for a glyph name of make_xbox_prompts.GLYPHS.
"""
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

CELL = 128
SCALE = 4  # drawn at 512 px and scaled down: smooth edges
S = CELL * SCALE
FONT = Path(__file__).resolve().parent / "fonts" / "LiberationSans-Bold.ttf"

FACE_COLOURS = {"a": (92, 214, 92), "b": (255, 70, 60), "x": (64, 150, 255), "y": (255, 205, 50)}
TEXT = (232, 232, 232)
HIGHLIGHT = (245, 245, 245)


def _font(size):
    return ImageFont.truetype(str(FONT), size)


def _mask(draw_shape):
    """An L mask of the canvas size with the shape drawn white."""
    mask = Image.new("L", (S, S), 0)
    draw_shape(ImageDraw.Draw(mask))
    return mask


def _gradient(top, bottom, box):
    """A vertical gradient from top to bottom colour across box (y0..y1), RGBA canvas."""
    y0, y1 = box
    column = Image.new("RGBA", (1, S))
    for y in range(S):
        t = min(1.0, max(0.0, (y - y0) / max(1, y1 - y0)))
        column.putpixel((0, y), tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    return column.resize((S, S))


_gradients = {}


def _cached_gradient(top, bottom, box):
    key = (top, bottom, box)
    if key not in _gradients:
        _gradients[key] = _gradient(top, bottom, box)
    return _gradients[key]


def _dark_body(canvas, mask, y0, y1):
    """The dark glossy finish inside mask: a gradient, a dark rim and a soft highlight on top."""
    body = _cached_gradient((70, 70, 72), (14, 14, 16), (y0, y1))
    canvas.paste(body, (0, 0), mask)
    # Rim: the shape's edge, darker.
    inner = mask.filter(ImageFilter.MinFilter(13))
    rim = ImageChops.subtract(mask, inner)
    canvas.paste((6, 6, 8, 255), (0, 0), rim)
    # Highlight: the upper part of the shape, faint white, blurred.
    shine = Image.new("L", (S, S), 0)
    h = y0 + (y1 - y0) * 0.45
    ImageDraw.Draw(shine).rectangle([0, 0, S, h], fill=70)
    shine = ImageChops.multiply(shine, inner.filter(ImageFilter.MinFilter(21)))
    shine = shine.filter(ImageFilter.GaussianBlur(18))
    white = Image.new("RGBA", (S, S), (255, 255, 255, 255))
    white.putalpha(shine)
    canvas.alpha_composite(white)


def _text(canvas, text, size, colour, centre, outline=(8, 8, 10), stroke=10):
    draw = ImageDraw.Draw(canvas)
    draw.text(centre, text, font=_font(size), fill=colour + (255,), anchor="mm",
              stroke_width=stroke, stroke_fill=outline + (255,))


def _glow(canvas, mask, colour, radius=26, strength=200):
    glow = mask.filter(ImageFilter.MaxFilter(17)).filter(ImageFilter.GaussianBlur(radius))
    glow = glow.point(lambda v: min(255, v * strength // 255))
    layer = Image.new("RGBA", (S, S), colour + (255,))
    layer.putalpha(glow)
    canvas.alpha_composite(layer)


def _circle(r, cx=S // 2, cy=S // 2):
    return _mask(lambda d: d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255))


def face(letter):
    colour = FACE_COLOURS[letter]
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    r = 196
    dome = _circle(r)
    _glow(canvas, dome, colour)
    _dark_body(canvas, dome, S // 2 - r, S // 2 + r)
    # The letter: colour, lighter at the top, dark outline.
    _text(canvas, letter.upper(), 250, colour, (S // 2, S // 2 + 8), stroke=12)
    return canvas


def small_round(symbol):
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    r = 170
    dome = _circle(r)
    _glow(canvas, dome, (200, 200, 200), radius=20, strength=70)
    _dark_body(canvas, dome, S // 2 - r, S // 2 + r)
    draw = ImageDraw.Draw(canvas)
    c = S // 2
    if symbol == "menu":
        for dy in (-56, 0, 56):
            draw.rounded_rectangle([c - 90, c + dy - 17, c + 90, c + dy + 17], radius=12,
                                   fill=TEXT + (255,))
    else:  # view: two overlapping windows
        draw.rounded_rectangle([c - 88, c - 78, c + 30, c + 22], radius=14, outline=TEXT + (255,),
                               width=20)
        draw.rounded_rectangle([c - 34, c - 26, c + 88, c + 78], radius=14, fill=(20, 20, 22, 255),
                               outline=TEXT + (255,), width=20)
    return canvas


def bumper(label):
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    box = [36, 136, S - 36, S - 136]
    shape = _mask(lambda d: d.rounded_rectangle(box, radius=110, fill=255))
    _glow(canvas, shape, (200, 200, 200), radius=18, strength=60)
    _dark_body(canvas, shape, box[1], box[3])
    _text(canvas, label, 180, TEXT, (S // 2, S // 2 + 6))
    return canvas


def trigger(label):
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    left = label == "LT"

    def draw_shape(d):
        # A trigger: rounded rectangle with the outer top corner pulled round.
        d.rounded_rectangle([70, 70, S - 70, S - 70], radius=70, fill=255)
        if left:
            d.pieslice([70, 70, 70 + 300, 70 + 300], 180, 270, fill=255)
        else:
            d.pieslice([S - 370, 70, S - 70, 370], 270, 360, fill=255)
    shape = _mask(draw_shape)
    corner = _mask(lambda d: d.rectangle([0, 0, 220, 220] if left else [S - 220, 0, S, 220], fill=255))
    round_corner = _mask(lambda d: d.ellipse([70, 70, 70 + 300, 370] if left else
                                             [S - 370, 70, S - 70, 370], fill=255))
    shape = ImageChops.subtract(shape, ImageChops.subtract(corner, round_corner))
    _glow(canvas, shape, (200, 200, 200), radius=18, strength=60)
    _dark_body(canvas, shape, 70, S - 70)
    _text(canvas, label, 175, TEXT, (S // 2 + (12 if left else -12), S // 2 + 30))
    return canvas


def stick(side, press):
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    cy = S // 2 + (40 if press else 0)
    r = 176 if press else 196
    dome = _circle(r, cy=cy)
    _glow(canvas, dome, (200, 200, 200), radius=20, strength=70)
    _dark_body(canvas, dome, cy - r, cy + r)
    draw = ImageDraw.Draw(canvas)
    c = S // 2
    ring = r - 52
    draw.ellipse([c - ring, cy - ring, c + ring, cy + ring], outline=(95, 95, 100, 255), width=12)
    _text(canvas, side, 170, TEXT, (c, cy + 6))
    if press:
        draw.polygon([(c - 70, 26), (c + 70, 26), (c, 104)], fill=HIGHLIGHT + (255,),
                     outline=(8, 8, 10, 255))
    return canvas


def dpad(lit):
    """lit: the arms drawn light (up, down, left, right)."""
    canvas = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    c, half, reach = S // 2, 78, 212
    arms = {
        "up": [c - half, c - reach, c + half, c],
        "down": [c - half, c, c + half, c + reach],
        "left": [c - reach, c - half, c, c + half],
        "right": [c, c - half, c + reach, c + half],
    }

    def cross(d):
        for box in arms.values():
            d.rounded_rectangle(box, radius=34, fill=255)
        d.rectangle([c - half, c - half, c + half, c + half], fill=255)
    shape = _mask(cross)
    _glow(canvas, shape, (200, 200, 200), radius=18, strength=60)
    _dark_body(canvas, shape, c - reach, c + reach)
    inset, inner = 26, 40
    for name in lit:
        x0, y0, x1, y1 = arms[name]
        box = [x0 + (inner if name == "right" else inset), y0 + (inner if name == "down" else inset),
               x1 - (inner if name == "left" else inset), y1 - (inner if name == "up" else inset)]
        ImageDraw.Draw(canvas).rounded_rectangle(box, radius=22, fill=HIGHLIGHT + (255,))
    return canvas


def draw(name):
    if name.startswith("button_color_"):
        image = face(name[-1])
    elif name in ("button_menu", "button_view"):
        image = small_round(name.split("_")[1])
    elif name in ("lb", "rb"):
        image = bumper(name.upper())
    elif name in ("lt", "rt"):
        image = trigger(name.upper())
    elif name.startswith("stick_"):
        image = stick(name[6].upper(), name.endswith("_press"))
    elif name.startswith("dpad"):
        lit = {"dpad": [], "dpad_up": ["up"], "dpad_down": ["down"], "dpad_left": ["left"],
               "dpad_right": ["right"], "dpad_horizontal": ["left", "right"],
               "dpad_vertical": ["up", "down"]}[name]
        image = dpad(lit)
    else:
        raise ValueError(name)
    return image.resize((CELL, CELL), Image.LANCZOS)
