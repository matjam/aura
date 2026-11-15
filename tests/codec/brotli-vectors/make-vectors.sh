#!/bin/sh
# Regenerate the brotli test vectors in this folder with the brotli command
# line tool (Homebrew: brew install brotli). ctest does not need it; the
# vectors are committed.
#
# NAME is an original; NAME.qQwW.br is NAME compressed at quality Q with
# window W. gen-words has no committed original: test_brotli builds it
# with the same generator (gen_words below and in test_brotli.c).
# The craft-* streams come from ../brotli_craft.py.
set -eu
cd "$(dirname "$0")"
root=../../..

: >empty
printf 'A' >one
cat "$root/CLAUDE.md" "$root/docs/v1-scope.md" "$root/README.md" >text

python3 -I - <<'EOF'
import random

# HTML built from the text, with the markup a page carries.
text = open("text", encoding="utf-8").read()
paras = [p.strip() for p in text.split("\n\n") if p.strip()]
out = ['<!DOCTYPE html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n'
       '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
       '<title>Aura notes</title>\n<link rel="stylesheet" href="/style.css">\n'
       '<script type="text/javascript" src="/main.js"></script>\n</head>\n<body>\n'
       '<div id="header" class="header"><a href="https://www.example.com/">Home</a></div>\n']
for i, p in enumerate(paras):
    p = p.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    if p.startswith("#"):
        out.append('<h2 class="section" id="s%d">%s</h2>\n' % (i, p.lstrip("# ")))
    else:
        out.append('<div class="para"><p>%s</p>\n<a href="/page/%d.html" title="Next">'
                   'More</a></div>\n' % (p, i))
out.append('<table border="0" cellpadding="0" cellspacing="0">\n')
for i in range(40):
    out.append('<tr><td class="c%d">%d</td><td><a href="#s%d">Section %d</a></td></tr>\n'
               % (i % 3, i, i, i))
out.append('</table>\n</body>\n</html>\n')
open("page.html", "w", encoding="utf-8").write("".join(out))

r = random.Random(7932)
open("random", "wb").write(bytes(r.getrandbits(8) for _ in range(6000)))

# Text in Greek, Cyrillic, Devanagari and CJK: two-, three- and four-byte
# UTF-8 sequences in literal contexts.
ranges = [(0x391, 0x3C9), (0x410, 0x44F), (0x905, 0x939), (0x4E00, 0x4FFF), (0x1F600, 0x1F64F)]
words = []
for _ in range(400):
    lo, hi = r.choice(ranges)
    words.append("".join(chr(r.randint(lo, hi)) for _ in range(r.randint(2, 7))))
text_u = []
while sum(len(w.encode()) + 1 for w in text_u) < 12000:
    text_u.append(r.choice(words) if r.random() < 0.9 else "aura")
open("utf8", "w", encoding="utf-8").write(" ".join(text_u))

# 16-bit signed samples: a sine with noise, for the signed context mode.
import math
samples = bytearray()
for i in range(3000):
    v = int(9000 * math.sin(i / 23.0) + r.randint(-300, 300))
    samples += (v & 0xFFFF).to_bytes(2, "little")
open("samples", "wb").write(bytes(samples))

# Long runs and short repeats, for overlapping copies.
rep = b"a" * 3000 + b"ab" * 2000 + b"abc" * 1500 + bytes(range(256)) * 20
open("repeat", "wb").write(rep)


def gen_words(n):
    words = [b"the ", b"window ", b"of ", b"ring\n"]
    x = 1
    out = bytearray()
    while len(out) < n:
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out += words[(x >> 16) & 3]
    return bytes(out[:n])


# Larger than a 64 KB window, so 16-bit windows wrap.
open("gen-words", "wb").write(gen_words(200000))
EOF

c() { brotli -f -q "$2" -w "$3" -o "$1.q$2w$3.br" "$1"; }
c empty 0 22
c empty 11 22
c one 0 22
c one 5 10
c one 11 24
for q in 0 5 9 11; do
    for w in 10 16; do c text $q $w; done
done
c text 1 16
for q in 5 11; do c text $q 22; done
c text 11 24
for q in 1 5 11; do c page.html $q 16; done
c page.html 9 10
c random 0 22
c random 11 10
for q in 0 5 11; do c repeat $q 16; done
for q in 5 9 11; do c utf8 $q 16; done
for q in 5 11; do c samples $q 16; done
c gen-words 9 16
c gen-words 5 24
rm gen-words

python3 -I ../brotli_craft.py .
for f in craft-*.br; do brotli -d -f -o "${f%.br}" "$f"; done
