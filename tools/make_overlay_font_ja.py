#!/usr/bin/env python3
"""The in-game menu's Japanese font: Noto Sans JP (SIL OFL 1.1) cut down to the characters the Japanese
translations use (gpu/shim/overlay_i18n.py), plus all kana and common Japanese punctuation, with TrueType outlines
(stb_truetype, which ImGui uses, reads them best). Run it again after new Japanese text:

    python tools/make_overlay_font_ja.py NotoSansJP-Regular.otf

NotoSansJP-Regular.otf: https://github.com/notofonts/noto-cjk (Sans/SubsetOTF/JP). Needs fontTools.
Writes gpu/third_party/fonts/NotoSansJP-overlay.ttf.
"""
import importlib.util
import sys
from pathlib import Path

from fontTools import subset
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont, newTable

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / 'gpu' / 'shim' / 'overlay_i18n.py'
OUTPUT = ROOT / 'gpu' / 'third_party' / 'fonts' / 'NotoSansJP-overlay.ttf'

KANA = [chr(c) for c in range(0x3041, 0x3097)] + [chr(c) for c in range(0x30A1, 0x3100)]
PUNCTUATION = '、。「」『』・ー～？！：（）［］【】〜…％／'


def characters():
    spec = importlib.util.spec_from_file_location('overlay_i18n', DATA)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    used = {ch for row in module.TRANSLATIONS.values() for ch in row.get('ja', '')}
    return sorted({ch for ch in used | set(KANA) | set(PUNCTUATION) if ord(ch) > 0x7F})


def to_truetype(font):
    """CFF outlines to TrueType quadratic outlines (glyf/loca)."""
    order = font.getGlyphOrder()
    glyph_set = font.getGlyphSet()
    glyf = newTable('glyf')
    glyf.glyphOrder = order
    glyf.glyphs = {}
    for name in order:
        pen = TTGlyphPen(glyph_set)
        glyph_set[name].draw(Cu2QuPen(pen, max_err=1.0, reverse_direction=True))
        glyf.glyphs[name] = pen.glyph()
    font['loca'] = newTable('loca')
    font['glyf'] = glyf
    del font['CFF ']
    if 'VORG' in font:
        del font['VORG']
    hmtx = font['hmtx']
    for name in order:
        glyph = glyf.glyphs[name]
        glyph.recalcBounds(glyf)
        advance, _lsb = hmtx[name]
        hmtx[name] = (advance, getattr(glyph, 'xMin', 0) if glyph.numberOfContours else 0)
    maxp = newTable('maxp')
    maxp.tableVersion = 0x00010000
    for field in ('maxZones', 'maxTwilightPoints', 'maxStorage', 'maxFunctionDefs', 'maxInstructionDefs',
                  'maxStackElements', 'maxSizeOfInstructions', 'maxComponentElements', 'maxComponentDepth'):
        setattr(maxp, field, 0)
    maxp.maxZones = 1
    font['maxp'] = maxp
    post = font['post']
    post.formatType = 3.0  # no glyph names
    font['head'].glyphDataFormat = 0
    font.sfntVersion = '\x00\x01\x00\x00'


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    chars = characters()
    options = subset.Options()
    options.layout_features = []  # ImGui does no shaping
    options.name_IDs = ['*']
    options.notdef_outline = True
    options.hinting = False
    options.desubroutinize = True
    font = TTFont(sys.argv[1])
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=[ord(ch) for ch in chars])
    subsetter.subset(font)
    to_truetype(font)
    for table in ('GSUB', 'GPOS', 'GDEF', 'BASE', 'vhea', 'vmtx', 'DSIG'):
        if table in font:
            del font[table]
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    font.save(OUTPUT)
    # reload: maxp counts and bounds recomputed on save
    print(f'{OUTPUT.name}: {len(chars)} characters, {len(font.getGlyphOrder())} glyphs, {OUTPUT.stat().st_size} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
