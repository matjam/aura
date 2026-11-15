#!/usr/bin/env python3
"""Builds Aura's bundled fonts in assets/fonts from the upstream releases.

Usage: tools/fonts-prepare.py SRC [OUT] [--only=FAMILY]...

FAMILY is liberation, inter, dejavu, notoemoji or twemoji (all by
default).

SRC holds the unpacked upstream downloads (see assets/fonts/README.md for
the URLs):

  SRC/liberation-fonts-ttf-2.1.5/     Liberation 2.1.5 TTF release
  SRC/Inter-4.1/extras/ttf/           Inter 4.1 release zip, unpacked
  SRC/dejavu-fonts-ttf-2.37/ttf/      DejaVu 2.37 TTF release
  SRC/NotoEmoji[wght].ttf             Noto Emoji from google/fonts
  SRC/NotoEmoji-OFL.txt               its OFL.txt from the same folder
  SRC/Twemoji.Mozilla.ttf             mozilla/twemoji-colr release v0.7.0
  SRC/twemoji-colr-LICENSE.md         LICENSE.md of the same tag

OUT defaults to assets/fonts. The script needs fontTools (pyftsubset); it
is only needed to regenerate the committed fonts, not to build Aura.

Every face is reduced to what Aura's engine draws: TrueType outlines, the
cmap, metrics and the legacy 'kern' table. Hinting (fpgm, prep, cvt, glyph
instructions), OpenType layout (GSUB, GPOS, GDEF) and glyph names are
removed, and glyphs that only GSUB could reach are dropped. Liberation
carries the Reserved Font Name "Liberation", so its subsets are renamed
Aura Sans, Aura Serif and Aura Mono as the OFL requires. Noto Emoji is
published only as a variable font; its wght=400 instance is taken here.
Twemoji Mozilla (colour emoji) keeps its COLR, CPAL and GSUB tables, and
gets an ATTRIBUTION.txt for the CC BY 4.0 licence of its graphics.
"""
import os
import shutil
import sys

from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

# Latin, IPA, combining marks, Greek, Cyrillic, Latin Extended Additional,
# Greek Extended, punctuation and symbols, Latin ligatures, specials.
# Hebrew and Arabic are left out: Aura has no bidirectional layout yet.
TEXT_RANGES = [
    (0x0000, 0x036F), (0x0370, 0x03FF), (0x0400, 0x052F), (0x1D00, 0x1DBF),
    (0x1E00, 0x1FFF), (0x2000, 0x2BFF), (0x2C60, 0x2C7F), (0x2E00, 0x2E7F),
    (0xA720, 0xA7FF), (0xFB00, 0xFB06), (0xFE20, 0xFE2F), (0xFFF9, 0xFFFD),
]
# The last-resort face keeps everything but the right-to-left scripts.
RTL_RANGES = [(0x0590, 0x08FF), (0xFB1D, 0xFDFF), (0xFE70, 0xFEFF), (0x10800, 0x10FFF), (0x1E800, 0x1EFFF)]


def in_ranges(cp, ranges):
    return any(a <= cp <= b for a, b in ranges)


def reduce_font(font, unicodes):
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = []
    opts.legacy_kern = True
    opts.glyph_names = False
    opts.notdef_outline = True
    opts.name_IDs = ['*']
    opts.name_languages = [0x0409]
    opts.drop_tables += ['GSUB', 'GPOS', 'GDEF', 'FFTM', 'gasp', 'MATH', 'STAT', 'DSIG', 'vhea', 'vmtx',
                         'HVAR', 'VVAR', 'MVAR', 'meta']
    s = subset.Subsetter(opts)
    s.populate(unicodes=unicodes)
    s.subset(font)


def rename(font, old, new):
    """Replaces the family name old with new in every naming record."""
    for rec in font['name'].names:
        if rec.nameID in (0, 7, 13, 14):
            continue  # copyright, trademark and licence notices stay
        text = rec.toUnicode()
        if old in text or old.replace(' ', '') in text:
            text = text.replace(old, new).replace(old.replace(' ', ''), new.replace(' ', ''))
            rec.string = text


def save(font, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    font.save(path)
    print('%-48s %8d' % (os.path.relpath(path), os.path.getsize(path)))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--only=')]
    only = [a[7:] for a in sys.argv[1:] if a.startswith('--only=')]
    if not args:
        print(__doc__)
        return 2
    src = args[0]
    out = args[1] if len(args) > 1 else os.path.join(os.path.dirname(__file__), '..', 'assets', 'fonts')
    steps = (('liberation', liberation), ('inter', inter), ('dejavu', dejavu), ('notoemoji', notoemoji),
             ('twemoji', twemoji))
    for name, step in steps:
        if not only or name in only:
            step(src, out)
    return 0


def liberation(src, out):
    lib = os.path.join(src, 'liberation-fonts-ttf-2.1.5')
    four = ('Regular', 'Bold', 'Italic', 'BoldItalic')
    # Monospaced italics are rare on the web; Aura synthesizes them.
    for fam, new, styles in (('Sans', 'Aura Sans', four), ('Serif', 'Aura Serif', four),
                             ('Mono', 'Aura Mono', ('Regular', 'Bold'))):
        for style in styles:
            f = TTFont(os.path.join(lib, 'Liberation%s-%s.ttf' % (fam, style)))
            cps = [cp for cp in f.getBestCmap() if in_ranges(cp, TEXT_RANGES)]
            reduce_font(f, cps)
            rename(f, 'Liberation ' + fam, new)
            save(f, os.path.join(out, 'liberation', '%s-%s.ttf' % (new.replace(' ', ''), style)))
    shutil.copy(os.path.join(lib, 'LICENSE'), os.path.join(out, 'liberation', 'LICENSE'))


def inter(src, out):
    d = os.path.join(src, 'Inter-4.1', 'extras', 'ttf')
    for style in ('Regular', 'Bold', 'Italic', 'BoldItalic'):
        f = TTFont(os.path.join(d, 'Inter-%s.ttf' % style))
        cps = [cp for cp in f.getBestCmap() if in_ranges(cp, TEXT_RANGES)]
        reduce_font(f, cps)
        save(f, os.path.join(out, 'inter', 'Inter-%s.ttf' % style))
    shutil.copy(os.path.join(src, 'Inter-4.1', 'LICENSE.txt'), os.path.join(out, 'inter', 'LICENSE.txt'))


def dejavu(src, out):
    dv = os.path.join(src, 'dejavu-fonts-ttf-2.37')
    # The last-resort face draws symbols and scripts the page's fonts lack;
    # bold is synthesized from the regular face.
    for name in ('DejaVuSans',):
        f = TTFont(os.path.join(dv, 'ttf', name + '.ttf'))
        cps = [cp for cp in f.getBestCmap() if not in_ranges(cp, RTL_RANGES)]
        reduce_font(f, cps)
        save(f, os.path.join(out, 'dejavu', name + '.ttf'))
    shutil.copy(os.path.join(dv, 'LICENSE'), os.path.join(out, 'dejavu', 'LICENSE'))


def notoemoji(src, out):
    f = TTFont(os.path.join(src, 'NotoEmoji[wght].ttf'))
    f = instancer.instantiateVariableFont(f, {'wght': 400}, updateFontNames=True)
    reduce_font(f, list(f.getBestCmap()))
    save(f, os.path.join(out, 'notoemoji', 'NotoEmoji-Regular.ttf'))
    shutil.copy(os.path.join(src, 'NotoEmoji-OFL.txt'), os.path.join(out, 'notoemoji', 'OFL.txt'))


def twemoji(src, out):
    # Colour emoji: Twemoji Mozilla keeps its colour tables (COLR version
    # 0, CPAL) and the GSUB ligatures (ccmp) that join ZWJ sequences, flags,
    # keycaps and skin tones; hinting and glyph names go.
    f = TTFont(os.path.join(src, 'Twemoji.Mozilla.ttf'))
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = ['ccmp']
    opts.glyph_names = False
    opts.notdef_outline = True
    opts.name_IDs = ['*']
    opts.name_languages = [0x0409]
    opts.drop_tables += ['FFTM', 'gasp', 'GDEF']
    s = subset.Subsetter(opts)
    s.populate(unicodes=list(f.getBestCmap()))
    s.subset(f)
    save(f, os.path.join(out, 'twemoji', 'TwemojiMozilla.ttf'))
    shutil.copy(os.path.join(src, 'twemoji-colr-LICENSE.md'), os.path.join(out, 'twemoji', 'LICENSE.md'))
    with open(os.path.join(out, 'twemoji', 'ATTRIBUTION.txt'), 'w') as a:
        a.write(TWEMOJI_ATTRIBUTION)


TWEMOJI_ATTRIBUTION = """Twemoji Mozilla, as bundled with Aura

Emoji graphics: Twemoji, copyright 2020 Twitter, Inc and other
contributors, licensed under the Creative Commons Attribution 4.0
International licence (CC BY 4.0):
https://creativecommons.org/licenses/by/4.0/
Source of the graphics: https://github.com/twitter/twemoji

Font build: Twemoji Mozilla 0.7.0, copyright Mozilla Foundation,
licensed under the Apache License 2.0:
https://www.apache.org/licenses/LICENSE-2.0
Source: https://github.com/mozilla/twemoji-colr/releases/tag/v0.7.0

Changes made for Aura: hinting, glyph names and the FFTM, gasp and GDEF
tables were removed (tools/fonts-prepare.py). The emoji graphics are
unchanged.
"""


if __name__ == '__main__':
    sys.exit(main())
