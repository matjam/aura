#!/usr/bin/env python3
"""Fetches and builds the fonts the font engine's tests use.

Usage:
  tools/test-fonts.py fetch [DIR]   download the upstream fonts into DIR
                                    (default build/test-fonts)
  tools/test-fonts.py make [DIR]    rebuild tests/paint/fonts from DIR
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
        make(d)
    else:
        refs(d)
    return 0


if __name__ == '__main__':
    sys.exit(main())
