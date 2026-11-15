#!/usr/bin/env python3
"""Fetches and builds the fonts the font engine's tests use.

Usage:
  tools/test-fonts.py fetch [DIR]   download the upstream fonts into DIR
                                    (default build/test-fonts)
  tools/test-fonts.py make [DIR] [all|var|color|web|samples]
                                    rebuild tests/paint/fonts from DIR
                                    (web: WOFF and WOFF2 of the var subsets;
                                    samples: the web fonts of
                                    tests/page/samples/webfonts.html)
  tools/test-fonts.py refs [DIR]    write full-size reference instances
                                    into DIR/refs (optional test)

Development only: building Aura and running its tests needs neither this
script nor fontTools. The files `make` writes are committed; the paint
tests compare the engine against them. `refs` writes larger references
that the optional test paint_var_full uses when they are present.

Downloads (official sources only):
  rsms/inter release v4.1 (InterVariable.ttf, web/InterVariable.woff2)
  google/fonts ofl/opensans (OpenSans[wdth,wght].ttf)
  googlefonts/noto-emoji 2D/fonts/Noto-COLRv1.ttf
  mozilla/twemoji-colr release v0.7.0 (Twemoji.Mozilla.ttf)

`make` needs fontTools. WOFF2 output also needs a brotli compressor:
fontTools uses the brotli Python module when it is installed, else this
script runs the brotli command-line tool through a small shim.
"""
import io
import os
import subprocess
import sys
import types
import urllib.request
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT = os.path.join(ROOT, 'tests', 'paint', 'fonts')

SOURCES = {
    'InterVariable.ttf': ('zip', 'https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip',
                          'InterVariable.ttf'),
    'InterVariable.woff2': ('zip', 'https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip',
                            'web/InterVariable.woff2'),
    'Inter-LICENSE.txt': ('zip', 'https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip',
                          'LICENSE.txt'),
    'OpenSans[wdth,wght].ttf': ('raw', 'https://raw.githubusercontent.com/google/fonts/main/ofl/opensans/'
                                'OpenSans%5Bwdth,wght%5D.ttf'),
    'OpenSans-OFL.txt': ('raw', 'https://raw.githubusercontent.com/google/fonts/main/ofl/opensans/OFL.txt'),
    'Noto-COLRv1.ttf': ('raw', 'https://raw.githubusercontent.com/googlefonts/noto-emoji/main/2D/fonts/'
                        'Noto-COLRv1.ttf'),
    'Twemoji.Mozilla.ttf': ('raw', 'https://github.com/mozilla/twemoji-colr/releases/download/v0.7.0/'
                            'Twemoji.Mozilla.ttf'),
    'twemoji-colr-LICENSE.md': ('raw', 'https://raw.githubusercontent.com/mozilla/twemoji-colr/v0.7.0/LICENSE.md'),
    'noto-emoji-LICENSE': ('raw', 'https://raw.githubusercontent.com/googlefonts/noto-emoji/main/2D/fonts/LICENSE'),
}


def fetch(d):
    os.makedirs(d, exist_ok=True)
    zips = {}
    for name, src in SOURCES.items():
        path = os.path.join(d, name)
        if os.path.exists(path):
            continue
        print('fetching', name)
        if src[0] == 'zip':
            if src[1] not in zips:
                zips[src[1]] = urllib.request.urlopen(src[1]).read()
            with zipfile.ZipFile(io.BytesIO(zips[src[1]])) as z:
                data = z.read(src[2])
        else:
            data = urllib.request.urlopen(src[1]).read()
        with open(path, 'wb') as f:
            f.write(data)


def brotli_shim():
    """fontTools' WOFF2 writer imports brotli; provide it through the CLI."""
    try:
        import brotli  # noqa: F401
        return
    except ImportError:
        pass
    m = types.ModuleType('brotli')

    def compress(data, mode=0, quality=11, lgwin=22, lgblock=0):
        return subprocess.run(['brotli', '-c', '-q', str(quality), '-w', str(lgwin)], input=data,
                              stdout=subprocess.PIPE, check=True).stdout

    def decompress(data):
        return subprocess.run(['brotli', '-d', '-c'], input=data, stdout=subprocess.PIPE, check=True).stdout

    m.compress = compress
    m.decompress = decompress
    m.MODE_GENERIC, m.MODE_TEXT, m.MODE_FONT = 0, 1, 2
    sys.modules['brotli'] = m


# Glyphs the variable-font tests compare: plain letters, composites
# (accented letters), digits and punctuation.
VAR_TEXT = 'AÁÅÆaáåbegnoxyHQRS0123%&@—fi'


def subset_var(src, dst, text, drop=()):
    from fontTools import subset
    from fontTools.ttLib import TTFont
    f = TTFont(src)
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = []
    opts.glyph_names = False
    opts.notdef_outline = True
    opts.drop_tables += ['GSUB', 'GPOS', 'GDEF', 'DSIG', 'cvar'] + list(drop)
    s = subset.Subsetter(opts)
    s.populate(text=text)
    s.subset(f)
    f.save(dst)
    return dst


def instance(src, dst, loc):
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer
    f = TTFont(src)
    f = instancer.instantiateVariableFont(f, loc)
    f.save(dst)


def make_var(d, out, refs_name, specs):
    """specs: (base file in out, [loc dicts])"""
    lines = []
    for base, locs in specs:
        for loc in locs:
            tag = '-'.join('%s%d' % (k, v) for k, v in sorted(loc.items()))
            name = base.replace('.ttf', '') + '@' + tag + '.ttf'
            instance(os.path.join(out, base), os.path.join(out, name), loc)
            lines.append('%s\t%s\t%s\n' % (base, name, ','.join('%s=%g' % kv for kv in sorted(loc.items()))))
    with open(os.path.join(out, refs_name), 'w') as f:
        f.write('# variable font\tstatic instance (fontTools instancer)\taxis values\n')
        f.writelines(lines)


def make(d):
    import shutil
    os.makedirs(OUT, exist_ok=True)
    shutil.copy(os.path.join(d, 'Inter-LICENSE.txt'), os.path.join(OUT, 'Inter-LICENSE.txt'))
    shutil.copy(os.path.join(d, 'OpenSans-OFL.txt'), os.path.join(OUT, 'OpenSans-OFL.txt'))
    subset_var(os.path.join(d, 'InterVariable.ttf'), os.path.join(OUT, 'intervar.ttf'), VAR_TEXT)
    subset_var(os.path.join(d, 'InterVariable.ttf'), os.path.join(OUT, 'intervar-nohvar.ttf'), VAR_TEXT,
               drop=('HVAR',))
    subset_var(os.path.join(d, 'OpenSans[wdth,wght].ttf'), os.path.join(OUT, 'opensansvar.ttf'), VAR_TEXT)
    make_var(d, OUT, 'var-refs.txt', [
        ('intervar.ttf', [{'wght': 100, 'opsz': 14}, {'wght': 700, 'opsz': 14}, {'wght': 900, 'opsz': 32},
                          {'wght': 550, 'opsz': 20}, {'wght': 400, 'opsz': 32}]),
        ('intervar-nohvar.ttf', [{'wght': 700, 'opsz': 14}, {'wght': 250, 'opsz': 28}]),
        ('opensansvar.ttf', [{'wght': 300, 'wdth': 75}, {'wght': 800, 'wdth': 100}, {'wght': 600, 'wdth': 87.5},
                             {'wght': 400, 'wdth': 75}]),
    ])


EMOJI_TEXT = ('😀🎉🚀❤👍🏽🏻🇯🇵🇬🇧1#⃣️‍👨👩👧🏳🌈☺🦄'
              '\U0001F3F4\U000E0067\U000E0062\U000E0073\U000E0063\U000E0074\U000E007F')


def subset_color(src, dst, text):
    from fontTools import subset
    from fontTools.ttLib import TTFont
    f = TTFont(src)
    opts = subset.Options()
    opts.hinting = False
    opts.layout_features = ['ccmp', 'liga', 'rlig', 'locl', 'clig']
    opts.glyph_names = False
    opts.notdef_outline = True
    opts.drop_tables += ['vhea', 'vmtx', 'FFTM', 'gasp']
    s = subset.Subsetter(opts)
    s.populate(text=text)
    s.subset(f)
    f.save(dst)


def build_colr_test(dst):
    """A COLR v0 + v1 test font: every paint format, composite modes,
    clip boxes, the foreground colour, a cycle and deep nesting, on
    private-use code points U+E000 onwards."""
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.colorLib.builder import buildCOLR, buildCPAL

    def rect(x0, y0, x1, y1):
        p = TTGlyphPen(None)
        p.moveTo((x0, y0))
        p.lineTo((x0, y1))
        p.lineTo((x1, y1))
        p.lineTo((x1, y0))
        p.closePath()
        return p.glyph()

    names = ['.notdef', 'square', 'left', 'right', 'small']
    glyphs = {'.notdef': rect(0, 0, 500, 700), 'square': rect(0, 0, 1000, 1000), 'left': rect(0, 0, 500, 1000),
              'right': rect(500, 0, 1000, 1000), 'small': rect(250, 250, 750, 750)}
    bases = ['uniE%03X' % i for i in range(16)]
    for b in bases:
        glyphs[b] = TTGlyphPen(None).glyph()
    order = names + bases
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({0xE000 + i: b for i, b in enumerate(bases)})
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics({g: (1000, 0) for g in order})
    fb.setupHorizontalHeader(ascent=1000, descent=0)
    fb.setupOS2(sTypoAscender=1000, sTypoDescender=0, usWinAscent=1000, usWinDescent=0)
    fb.setupNameTable({'familyName': 'Aura COLR Test', 'styleName': 'Regular'})
    fb.setupPost()
    # Palette: 0 red, 1 blue, 2 green, 3 half-transparent white.
    fb.font['CPAL'] = buildCPAL([[(1, 0, 0, 1), (0, 0, 1, 1), (0, 1, 0, 1), (1, 1, 1, 0.5)]])

    def solid(i, a=1.0):
        return {'Format': 2, 'PaletteIndex': i, 'Alpha': a}

    def glyph(g, paint):
        return {'Format': 10, 'Paint': paint, 'Glyph': g}

    line = {'Extend': 'pad', 'ColorStop': [{'StopOffset': 0, 'PaletteIndex': 0, 'Alpha': 1},
                                           {'StopOffset': 1, 'PaletteIndex': 1, 'Alpha': 1}]}
    reflect = dict(line, Extend='reflect')
    chain = glyph('square', solid(2))
    for _ in range(60):
        chain = {'Format': 14, 'Paint': chain, 'dx': 0, 'dy': 0}
    color_glyphs = {
        'uniE000': [('left', 0), ('right', 1)],                       # v0 layers
        'uniE001': glyph('square', solid(0)),                         # solid
        'uniE002': glyph('square', {'Format': 4, 'ColorLine': line, 'x0': 0, 'y0': 0, 'x1': 1000, 'y1': 0,
                                    'x2': 0, 'y2': 1000}),            # linear, left red to right blue
        'uniE003': glyph('square', {'Format': 6, 'ColorLine': line, 'x0': 500, 'y0': 500, 'r0': 0,
                                    'x1': 500, 'y1': 500, 'r1': 500}),  # radial, red centre
        'uniE004': glyph('square', {'Format': 8, 'ColorLine': line, 'centerX': 500, 'centerY': 500,
                                    'startAngle': 0, 'endAngle': 360}),  # sweep
        'uniE005': {'Format': 32, 'SourcePaint': glyph('small', solid(1)), 'CompositeMode': 'src_in',
                    'BackdropPaint': glyph('left', solid(0))},        # composite: blue only over the left half
        'uniE006': {'Format': 26, 'Paint': glyph('left', solid(2)), 'angle': 180, 'centerX': 500,
                    'centerY': 500},                                  # rotate: green on the right
        'uniE007': {'Format': 14, 'Paint': {'Format': 11, 'Glyph': 'uniE001'}, 'dx': 0, 'dy': 0},  # colr glyph
        'uniE008': glyph('square', solid(0xFFFF)),                    # foreground
        'uniE009': {'Format': 11, 'Glyph': 'uniE009'},                # a cycle
        'uniE00A': chain,                                             # deep nesting
        'uniE00B': {'Format': 1, 'Layers': [glyph('square', solid(0)), glyph('small', solid(1))]},
        'uniE00C': glyph('square', solid(1)),                         # clipped to the left half
        'uniE00D': {'Format': 28, 'Paint': glyph('small', solid(0)), 'xSkewAngle': 20, 'ySkewAngle': 0},
        'uniE00E': {'Format': 32, 'SourcePaint': glyph('square', solid(1)), 'CompositeMode': 'multiply',
                    'BackdropPaint': glyph('square', solid(0))},      # multiply: black
        'uniE00F': glyph('square', {'Format': 4, 'ColorLine': reflect, 'x0': 0, 'y0': 0, 'x1': 500, 'y1': 0,
                                    'x2': 0, 'y2': 1000}),            # reflect: red, blue, red
    }
    fb.font['COLR'] = buildCOLR(color_glyphs, glyphMap=fb.font.getReverseGlyphMap(),
                                clipBoxes={'uniE00C': (0, 0, 500, 1000)})
    fb.save(dst)


def make_color(d):
    import shutil
    shutil.copy(os.path.join(d, 'twemoji-colr-LICENSE.md'), os.path.join(OUT, 'Twemoji-LICENSE.md'))
    shutil.copy(os.path.join(d, 'noto-emoji-LICENSE'), os.path.join(OUT, 'NotoColorEmoji-OFL.txt'))
    subset_color(os.path.join(d, 'Twemoji.Mozilla.ttf'), os.path.join(OUT, 'twemoji-sub.ttf'), EMOJI_TEXT)
    subset_color(os.path.join(d, 'Noto-COLRv1.ttf'), os.path.join(OUT, 'notocolr-sub.ttf'), EMOJI_TEXT)
    build_colr_test(os.path.join(OUT, 'colr-test.ttf'))


def uint_base128(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def u255(n):
    import struct
    if n < 253:
        return bytes([n])
    if n < 506:
        return bytes([255, n - 253])
    if n < 762:
        return bytes([254, n - 506])
    return bytes([253]) + struct.pack('>H', n)


def woff2_collection(fonts, dst):
    """A WOFF2 collection of whole sfnt files, written here because
    fontTools has no collection writer: every table untransformed (glyf
    and loca with the null transform), the second font sharing the first
    one's 'post' table to exercise sharing."""
    import struct
    from fontTools.ttLib import TTFont
    from fontTools.ttLib.woff2 import woff2KnownTags
    import brotli
    tables = []   # (tag, data)
    dirs = []     # (flavor, [table indices])
    for i, path in enumerate(fonts):
        f = TTFont(path)
        reader = f.reader
        idx = []
        for tag in sorted(reader.keys()):
            data = reader[tag]
            if i > 0 and tag == 'post':
                idx.append(next(k for k, t in enumerate(tables) if t[0] == 'post'))
                continue
            idx.append(len(tables))
            tables.append((tag, data))
        dirs.append((reader.sfntVersion, idx))
    directory = b''
    stream = b''
    for tag, data in tables:
        known = woff2KnownTags.index(tag) if tag in woff2KnownTags else 63
        xform = 3 if tag in ('glyf', 'loca') else 0
        directory += bytes([known | (xform << 6)])
        if known == 63:
            directory += tag.encode('latin-1')
        directory += uint_base128(len(data))
        stream += data
    coll = struct.pack('>L', 0x00010000) + u255(len(dirs))
    for flavor, idx in dirs:
        fl = flavor if isinstance(flavor, bytes) else flavor.encode('latin-1')
        coll += u255(len(idx)) + fl + b''.join(u255(k) for k in idx)
    comp = brotli.compress(stream)
    body = directory + coll
    length = 48 + len(body) + len(comp)
    length += (-length) % 4
    head = struct.pack('>4s4sLHHLLHHLLLLL', b'wOF2', b'ttcf', length, len(tables), 0, len(stream), len(comp),
                       1, 0, 0, 0, 0, 0, 0)
    out = head + body + comp
    out += b'\0' * (length - len(out))
    with open(dst, 'wb') as f:
        f.write(out)


def make_web(d):
    """WOFF and WOFF2 versions of the variable test subsets."""
    from fontTools.ttLib import TTFont
    from fontTools.ttLib import woff2
    for name in ('intervar', 'opensansvar'):
        f = TTFont(os.path.join(OUT, name + '.ttf'))
        f.flavor = 'woff'
        f.save(os.path.join(OUT, name + '.woff'))
    # glyf and loca transformed (the default), then hmtx too.
    woff2.compress(os.path.join(OUT, 'intervar.ttf'), os.path.join(OUT, 'intervar.woff2'))
    woff2.compress(os.path.join(OUT, 'opensansvar.ttf'), os.path.join(OUT, 'opensansvar-hmtx.woff2'),
                   transform_tables={'glyf', 'loca', 'hmtx'})
    woff2_collection([os.path.join(OUT, 'intervar.ttf'), os.path.join(OUT, 'opensansvar.ttf')],
                     os.path.join(OUT, 'collection.woff2'))


SAMPLES = os.path.join(ROOT, 'tests', 'page', 'samples')


def make_samples(d):
    """The web fonts of tests/page/samples/webfonts.html: Inter Variable
    (wght and opsz axes) reduced to ASCII as WOFF2, and Open Sans variable
    (wdth and wght) reduced to digits as WOFF."""
    import shutil
    from fontTools.ttLib import TTFont
    ascii_text = ''.join(chr(c) for c in range(0x20, 0x7F)) + '’“”—'
    tmp = os.path.join(d, 'sample-inter.ttf')
    subset_var(os.path.join(d, 'InterVariable.ttf'), tmp, ascii_text)
    woff2_path = os.path.join(SAMPLES, 'sample-inter.woff2')
    from fontTools.ttLib import woff2
    woff2.compress(tmp, woff2_path)
    tmp = os.path.join(d, 'sample-digits.ttf')
    subset_var(os.path.join(d, 'OpenSans[wdth,wght].ttf'), tmp, '0123456789')
    f = TTFont(tmp)
    f.flavor = 'woff'
    f.save(os.path.join(SAMPLES, 'sample-digits.woff'))
    shutil.copy(os.path.join(d, 'Inter-LICENSE.txt'), os.path.join(SAMPLES, 'sample-inter-OFL.txt'))
    shutil.copy(os.path.join(d, 'OpenSans-OFL.txt'), os.path.join(SAMPLES, 'sample-digits-OFL.txt'))


def refs(d):
    out = os.path.join(d, 'refs')
    os.makedirs(out, exist_ok=True)
    import shutil
    shutil.copy(os.path.join(d, 'InterVariable.ttf'), os.path.join(out, 'InterVariable.ttf'))
    shutil.copy(os.path.join(d, 'OpenSans[wdth,wght].ttf'), os.path.join(out, 'OpenSansVar.ttf'))
    make_var(d, out, 'var-refs.txt', [
        ('InterVariable.ttf', [{'wght': 300, 'opsz': 14}, {'wght': 800, 'opsz': 32}]),
        ('OpenSansVar.ttf', [{'wght': 700, 'wdth': 75}]),
    ])


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('fetch', 'make', 'refs'):
        print(__doc__)
        return 2
    d = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'build', 'test-fonts')
    if sys.argv[1] == 'fetch':
        fetch(d)
    elif sys.argv[1] == 'make':
        brotli_shim()
        what = sys.argv[3] if len(sys.argv) > 3 else 'all'
        if what in ('all', 'var'):
            make(d)
        if what in ('all', 'color'):
            make_color(d)
        if what in ('all', 'web'):
            make_web(d)
        if what in ('all', 'samples'):
            make_samples(d)
    else:
        refs(d)
    return 0


if __name__ == '__main__':
    sys.exit(main())
