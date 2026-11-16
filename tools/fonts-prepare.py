#!/usr/bin/env python3
"""Builds Aura's bundled fonts in assets/fonts from the upstream releases.

Usage: tools/fonts-prepare.py SRC [OUT] [--only=FAMILY]...

FAMILY is liberation, inter, dejavu, notoemoji, twemoji or cjk (all by
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
  SRC/NotoSansSC[wght].ttf            Noto Sans SC from google/fonts
  SRC/NotoSansKR[wght].ttf            Noto Sans KR from google/fonts
  SRC/NotoSansCJK-OFL.txt             their OFL.txt

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

The CJK faces are the wght=400 instances of Noto Sans SC and Noto Sans KR
subset to the characters of the common national standards (see cjk()):
Noto Sans SC holds the ideographs for Chinese, Japanese and Korean (Han
unification), kana, bopomofo and the CJK punctuation; Noto Sans KR holds
only hangul. The character sets come from Python's own codecs.
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
                         'HVAR', 'VVAR', 'MVAR', 'meta', 'VORG', 'BASE']
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
             ('twemoji', twemoji), ('cjk', cjk))
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


def double_byte(codec, leads, trails):
    """The characters of a double-byte character set, by decoding every
    lead and trail byte pair with one of Python's codecs."""
    out = set()
    for a in leads:
        for b in trails:
            try:
                t = bytes([a, b]).decode(codec)
            except UnicodeDecodeError:
                continue
            if len(t) == 1:
                out.add(ord(t))
    return out


def is_han(cp):
    return 0x3400 <= cp <= 0x9FFF or 0xF900 <= cp <= 0xFAFF or 0x20000 <= cp <= 0x3FFFF


# The CJK blocks a CJK face draws beyond the ideographs: radicals,
# punctuation, kana, bopomofo, enclosed and compatibility characters,
# vertical, compatibility and small forms, fullwidth and halfwidth forms.
CJK_BLOCKS = [(0x2E80, 0x33FF), (0xFE10, 0xFE1F), (0xFE30, 0xFE6F), (0xFF00, 0xFFEF)]


def cjk_sets():
    """The characters of the two CJK faces: (Noto Sans SC's, Noto Sans KR's).

    Ideographs: GB 2312 (6,763, simplified Chinese), the Big5 common set
    (level 1, 5,401, traditional Chinese) and the JIS X 0208 kanji (6,355),
    10,269 together. Hangul: the 2,350 syllables of KS X 1001. Beyond them,
    the characters of those standards in the CJK blocks, and all of kana,
    bopomofo, the CJK punctuation and the compatibility jamo."""
    big5_trail = list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF))
    gb = double_byte('gb2312', range(0xA1, 0xF8), range(0xA1, 0xFF))
    big5 = double_byte('big5', range(0xA1, 0xC7), big5_trail)  # symbols and level 1 (to C67E)
    jis = double_byte('euc_jp', range(0xA1, 0xF5), range(0xA1, 0xFF))
    ks = double_byte('euc_kr', range(0xA1, 0xFE), range(0xA1, 0xFF))
    han = {c for c in gb | big5 | jis if is_han(c)}
    blocks = {c for c in gb | big5 | jis | ks if in_ranges(c, CJK_BLOCKS)}
    whole = set(range(0x3000, 0x3130)) | set(range(0x31A0, 0x31C0)) | set(range(0x31F0, 0x3200))
    hangul = {c for c in ks if 0xAC00 <= c <= 0xD7A3}
    jamo = set(range(0x3131, 0x318F))
    # Hangul compatibility jamo and the circled hangul stay in the hangul
    # face, so Korean letters are drawn in one design.
    korean = jamo | set(range(0x3200, 0x321F)) | set(range(0x3260, 0x327F))
    return (han | blocks | whole) - korean, hangul | korean | blocks


def common_first(font):
    """Orders the glyphs of a CJK face so the most common characters lie
    together at the start of glyf, which a paged face reads in blocks:
    .notdef, the characters that are not ideographs, then ideographs in all
    three of GB 2312 level 1, JIS X 0208 level 1 and Big5 level 1, in two
    of them, in one, and the rest; each group in code point order. A whole
    Japanese or Chinese Wikipedia front page then touches about a third
    fewer blocks."""
    from fontTools.ttLib.reorderGlyphs import reorderGlyphs
    big5_trail = list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF))
    levels = (double_byte('gb2312', range(0xB0, 0xD8), range(0xA1, 0xFF)),
              double_byte('euc_jp', range(0xB0, 0xD0), range(0xA1, 0xFF)),
              double_byte('big5', range(0xA4, 0xC7), big5_trail))
    char_of = {}
    for cp, name in font.getBestCmap().items():
        char_of[name] = min(cp, char_of.get(name, cp))
    order = font.getGlyphOrder()

    def key(name):
        if name == order[0]:
            return (0, 0, 0)
        cp = char_of.get(name)
        if cp is None or not is_han(cp):
            return (1, 0, cp or 0)
        n = sum(cp in level for level in levels)
        return (2, 3 - n, cp)
    reorderGlyphs(font, sorted(order, key=key))


def cjk(src, out):
    sc, kr = cjk_sets()
    d = os.path.join(out, 'notosanscjk')
    for name, want in (('SC', sc), ('KR', kr)):
        f = TTFont(os.path.join(src, 'NotoSans%s[wght].ttf' % name))
        f = instancer.instantiateVariableFont(f, {'wght': 400}, updateFontNames=True)
        cmap = f.getBestCmap()
        if name == 'KR':
            # Hangul and what Noto Sans SC lacks: the SC face is tried first.
            want = {c for c in want if 0xAC00 <= c <= 0xD7A3 or 0x3131 <= c <= 0x318E or c not in sc_have}
        cps = sorted(c for c in want if c in cmap)
        reduce_font(f, cps)
        if name == 'SC':
            sc_have = set(cps)
            common_first(f)
        save(f, os.path.join(d, 'NotoSans%s-Regular.ttf' % name))
        print('  %d of %d characters' % (len(cps), len(want)))
    shutil.copy(os.path.join(src, 'NotoSansCJK-OFL.txt'), os.path.join(d, 'OFL.txt'))


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
