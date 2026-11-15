#!/usr/bin/env python3
"""Write hand-made brotli streams for tests/codec/brotli-vectors.

Encoders never produce some parts of the format, or produce them only by
chance: every one of the 121 dictionary transforms, nonzero NPOSTFIX and
NDIRECT, metadata blocks, an uncompressed meta-block followed by a
compressed one, the smallest window. This script builds streams that use
them with simple prefix codes. The expected output of each is what the
reference decoder (the brotli command line tool) makes of it, so a mistake
here shows up as a mismatch rather than as a wrong expectation:

    python3 -I tests/codec/brotli_craft.py tests/codec/brotli-vectors
    for f in tests/codec/brotli-vectors/craft-*.br; do
        brotli -d -f -o "${f%.br}" "$f"
    done

To regenerate the encoder vectors, see make-vectors.sh in that folder.
"""
import os
import sys


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, v, n):
        assert 0 <= v < (1 << n) or n == 0
        self.acc |= v << self.n
        self.n += n
        while self.n >= 8:
            self.out.append(self.acc & 0xFF)
            self.acc >>= 8
            self.n -= 8

    def align(self):
        if self.n:
            self.put(0, 8 - self.n)

    def data(self):
        self.align()
        return bytes(self.out)


def alpha_bits(size):
    n = 0
    while (1 << n) < size:
        n += 1
    return n


def simple_code(w, alphabet, syms):
    """A simple prefix code with 1 to 3 symbols (lengths 0, 1-1, 1-2-2)."""
    w.put(1, 2)
    w.put(len(syms) - 1, 2)
    for s in syms:
        w.put(s, alpha_bits(alphabet))


def wbits(w, n):
    if n == 16:
        w.put(0, 1)
    elif n > 17:
        w.put(1, 1)
        w.put(n - 17, 3)
    elif n == 17:
        w.put(1, 1)
        w.put(0, 3)
        w.put(0, 3)
    else:
        w.put(1, 1)
        w.put(0, 3)
        w.put(n - 8, 3)


def mlen(w, n):
    n -= 1
    nib = 4
    while n >> (4 * nib):
        nib += 1
    assert nib <= 6
    w.put(nib - 4, 2)
    w.put(n, 4 * nib)


COPY_BASE = [2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118]
COPY_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24]


def copy_code(length):
    for c in range(24):
        if COPY_BASE[c] <= length < COPY_BASE[c] + (1 << COPY_EXTRA[c]):
            return c, length - COPY_BASE[c]
    raise ValueError(length)


def dist_code(d, npostfix, ndirect):
    """(symbol, extra bits value, extra bit count) for distance d."""
    if d <= ndirect:
        return d + 15, 0, 0
    x = d - ndirect - 1
    lcode = x & ((1 << npostfix) - 1)
    y = x >> npostfix
    for nb in range(1, 25):
        for h1 in (0, 1):
            off = ((2 + h1) << nb) - 4
            if off <= y < off + (1 << nb):
                hcode = ((nb - 1) << 1) | h1
                return ((hcode << npostfix) | lcode) + ndirect + 16, y - off, nb
    raise ValueError(d)


NDBITS = [0, 0, 0, 0, 10, 10, 11, 11, 10, 10, 10, 10, 10, 9, 9, 8, 7, 7, 8, 7, 7, 6, 6, 5, 5]

# Output length of each transform, given the word length: prefix length,
# omitted bytes (negative: from the end) and suffix length. Only the
# lengths matter here; the reference decoder supplies the bytes.
T_ID, T_UF, T_UA = 0, 100, 101


def OL(n):
    return -n


def OF(n):
    return n


TRANSFORMS = [
    ("", T_ID, ""), ("", T_ID, " "), (" ", T_ID, " "), ("", OF(1), ""),
    ("", T_UF, " "), ("", T_ID, " the "), (" ", T_ID, ""), ("s ", T_ID, " "),
    ("", T_ID, " of "), ("", T_UF, ""), ("", T_ID, " and "), ("", OF(2), ""),
    ("", OL(1), ""), (", ", T_ID, " "), ("", T_ID, ", "), (" ", T_UF, " "),
    ("", T_ID, " in "), ("", T_ID, " to "), ("e ", T_ID, " "), ("", T_ID, "\""),
    ("", T_ID, "."), ("", T_ID, "\">"), ("", T_ID, "\n"), ("", OL(3), ""),
    ("", T_ID, "]"), ("", T_ID, " for "), ("", OF(3), ""), ("", OL(2), ""),
    ("", T_ID, " a "), ("", T_ID, " that "), (" ", T_UF, ""), ("", T_ID, ". "),
    (".", T_ID, ""), (" ", T_ID, ", "), ("", OF(4), ""), ("", T_ID, " with "),
    ("", T_ID, "'"), ("", T_ID, " from "), ("", T_ID, " by "), ("", OF(5), ""),
    ("", OF(6), ""), (" the ", T_ID, ""), ("", OL(4), ""), ("", T_ID, ". The "),
    ("", T_UA, ""), ("", T_ID, " on "), ("", T_ID, " as "), ("", T_ID, " is "),
    ("", OL(7), ""), ("", OL(1), "ing "), ("", T_ID, "\n\t"), ("", T_ID, ":"),
    (" ", T_ID, ". "), ("", T_ID, "ed "), ("", OF(9), ""), ("", OF(7), ""),
    ("", OL(6), ""), ("", T_ID, "("), ("", T_UF, ", "), ("", OL(8), ""),
    ("", T_ID, " at "), ("", T_ID, "ly "), (" the ", T_ID, " of "), ("", OL(5), ""),
    ("", OL(9), ""), (" ", T_UF, ", "), ("", T_UF, "\""), (".", T_ID, "("),
    ("", T_UA, " "), ("", T_UF, "\">"), ("", T_ID, "=\""), (" ", T_ID, "."),
    (".com/", T_ID, ""), (" the ", T_ID, " of the "), ("", T_UF, "'"), ("", T_ID, ". This "),
    ("", T_ID, ","), (".", T_ID, " "), ("", T_UF, "("), ("", T_UF, "."),
    ("", T_ID, " not "), (" ", T_ID, "=\""), ("", T_ID, "er "), (" ", T_UA, " "),
    ("", T_ID, "al "), (" ", T_UA, ""), ("", T_ID, "='"), ("", T_UA, "\""),
    ("", T_UF, ". "), (" ", T_ID, "("), ("", T_ID, "ful "), (" ", T_UF, ". "),
    ("", T_ID, "ive "), ("", T_ID, "less "), ("", T_UA, "'"), ("", T_ID, "est "),
    (" ", T_UF, "."), ("", T_UA, "\">"), (" ", T_ID, "='"), ("", T_UF, ","),
    ("", T_ID, "ize "), ("", T_UA, "."), ("\xc2\xa0", T_ID, ""), (" ", T_ID, ","),
    ("", T_UF, "=\""), ("", T_UA, "=\""), ("", T_ID, "ous "), ("", T_UA, ", "),
    ("", T_UF, "='"), (" ", T_UF, ","), (" ", T_UA, "=\""), (" ", T_UA, ", "),
    ("", T_UA, ","), ("", T_UA, "("), ("", T_UA, ". "), (" ", T_UA, "."),
    ("", T_UA, "='"), (" ", T_UA, ". "), (" ", T_UF, "=\""), (" ", T_UA, "='"),
    (" ", T_UF, "='"),
]
assert len(TRANSFORMS) == 121


def out_len(t, length):
    pre, ty, suf = TRANSFORMS[t]
    plen = 2 if pre == "\xc2\xa0" else len(pre)
    if ty in (T_ID, T_UF, T_UA):
        w = length
    elif ty > 0:
        w = max(0, length - ty)
    else:
        w = max(0, length + ty)
    return plen + w + len(suf)


def dict_block(w, total, word_len, idx, t, npostfix, ndirect_code, last):
    """One meta-block holding one dictionary reference; returns its
    output length."""
    n = out_len(t, word_len)
    assert n > 0
    ndirect = ndirect_code << npostfix
    w.put(1 if last else 0, 1)
    if last:
        w.put(0, 1)
    mlen(w, n)
    if not last:
        w.put(0, 1)  # compressed
    w.put(0, 1)  # NBLTYPESL = 1
    w.put(0, 1)  # NBLTYPESI = 1
    w.put(0, 1)  # NBLTYPESD = 1
    w.put(npostfix, 2)
    w.put(ndirect_code, 4)
    w.put(0, 2)  # CMODE
    w.put(0, 1)  # NTREESL = 1
    w.put(0, 1)  # NTREESD = 1
    simple_code(w, 256, [ord("x")])
    cc, cextra = copy_code(word_len)
    if cc < 8:
        cmd = 128 + cc
    elif cc < 16:
        cmd = 192 + cc - 8
    else:
        cmd = 384 + cc - 16
    simple_code(w, 704, [cmd])
    alpha = 16 + ndirect + (48 << npostfix)
    max_dist = min((1 << 16) - 16, total)
    word_id = (t << NDBITS[word_len]) | idx
    dcode, dextra, dnb = dist_code(max_dist + 1 + word_id, npostfix, ndirect)
    assert dcode < alpha
    simple_code(w, alpha, [dcode])
    # The command: insert code 0 (no extra bits), copy extra, distance extra
    # (the prefix codes have one symbol each, so they take no bits).
    w.put(cextra, COPY_EXTRA[cc])
    w.put(dextra, dnb)
    return n


def craft_dict(path):
    w = Bits()
    wbits(w, 16)
    total = 0
    refs = []
    for t in range(121):
        for length in (4 + t % 21, 24 - t % 21):
            idx = (t * 37 + length * 11) % (1 << NDBITS[length])
            refs.append((length, idx, t))
    # Words near the end of each length are not ASCII: uppercase them.
    for length in range(4, 25):
        for k in (1, 2, 5, 9):
            for t in (9, 44, 4, 68):
                refs.append((length, (1 << NDBITS[length]) - k, t))
    refs = [r for r in refs if out_len(r[2], r[0]) > 0]
    for i, (length, idx, t) in enumerate(refs):
        total += dict_block(w, total, length, idx, t, i % 4, (i * 5) % 16, False)
    w.put(1, 1)
    w.put(1, 1)
    open(path, "wb").write(w.data())


def craft_edge(path):
    """WBITS 10; two metadata blocks; an uncompressed meta-block; a
    compressed last meta-block with literals and a copy reaching back into
    the uncompressed data."""
    w = Bits()
    wbits(w, 10)
    # Metadata with 3 bytes.
    w.put(0, 1)
    w.put(3, 2)
    w.put(0, 1)
    w.put(1, 2)
    w.put(2, 8)
    w.align()
    for c in b"abc":
        w.put(c, 8)
    # Empty metadata.
    w.put(0, 1)
    w.put(3, 2)
    w.put(0, 1)
    w.put(0, 2)
    w.align()
    # Uncompressed "Hello, ".
    w.put(0, 1)
    mlen(w, 7)
    w.put(1, 1)
    w.align()
    for c in b"Hello, ":
        w.put(c, 8)
    # Last: insert "ab", copy 5 from distance 9 ("Hello").
    w.put(1, 1)
    w.put(0, 1)
    mlen(w, 7)
    for _ in range(3):
        w.put(0, 1)
    w.put(0, 2)
    w.put(0, 4)
    w.put(0, 2)
    w.put(0, 1)
    w.put(0, 1)
    simple_code(w, 256, [ord("a"), ord("b")])
    simple_code(w, 704, [128 + (2 << 3) + 3])
    dcode, dextra, dnb = dist_code(9, 0, 0)
    simple_code(w, 64, [dcode])
    w.put(0, 1)  # literal 'a'
    w.put(1, 1)  # literal 'b'
    w.put(dextra, dnb)
    open(path, "wb").write(w.data())


def craft_uncompressed(path):
    """WBITS 24; a non-last uncompressed meta-block, then an empty last
    one: the decoder may size its ring to the data."""
    w = Bits()
    wbits(w, 24)
    data = bytes((i * 7 + 3) & 0xFF for i in range(300))
    w.put(0, 1)
    mlen(w, len(data))
    w.put(1, 1)
    w.align()
    for c in data:
        w.put(c, 8)
    w.put(1, 1)
    w.put(1, 1)
    open(path, "wb").write(w.data())


def varlen(w, n):
    """NBLTYPES / NTREES (1..256)."""
    if n == 1:
        w.put(0, 1)
        return
    w.put(1, 1)
    if n == 2:
        w.put(0, 3)
        return
    k = (n - 1).bit_length() - 1
    w.put(k, 3)
    w.put(n - 1 - (1 << k), k)


# Section 3.5: the code for code length code lengths, as (bits, count).
CLCL_CODE = {0: (0, 2), 1: (7, 4), 2: (3, 3), 3: (2, 2), 4: (1, 2), 5: (15, 4)}
CL_ORDER = [1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15]


def complex_code_7(w):
    """A complex prefix code for 7 symbols with lengths 2,3,3,3,3,3,3:
    code length symbols 2 and 3 get one bit each."""
    w.put(0, 2)  # HSKIP 0
    for sym in CL_ORDER[:3]:
        v = {2: 1, 3: 1}.get(sym, 0)
        w.put(*CLCL_CODE[v])
    for ln in [2, 3, 3, 3, 3, 3, 3]:
        w.put(0 if ln == 2 else 1, 1)


def complex_code_single_cl(w):
    """A complex prefix code whose code length code has one symbol (2):
    every literal code length is 2 and takes no bits, so symbols 0..3 get
    2-bit codes."""
    w.put(0, 2)
    for sym in CL_ORDER:
        w.put(*CLCL_CODE[1 if sym == 2 else 0])


def craft_blocks(path, rnd):
    """Block types and switches in all three categories (every kind of
    block type code), all four context modes, a literal context map with
    RLE and IMTF, a distance context map, and a complex prefix code with a
    single code length code. Each literal costs 2 bits whatever tree the
    context picks, so the writer need not know the contexts."""
    ncmds = 300
    w = Bits()
    wbits(w, 18)
    w.put(1, 1)
    w.put(0, 1)
    mlen(w, ncmds * 7)
    # Literals: 4 types; type codes 0, 1, 3, 5 (2 bits each); counts
    # 1..8 (code 0 or 1, 2 extra bits).
    varlen(w, 4)
    w.put(1, 2), w.put(3, 2)
    for s in (0, 1, 3, 5):
        w.put(s, 3)
    w.put(0, 1)
    simple_code(w, 26, [0, 1])
    lcount = 3
    w.put(0, 1), w.put(lcount - 1, 2)  # code 0, extra
    # Insert-and-copy: 2 types, always "last + 1", counts 1..4.
    varlen(w, 2)
    simple_code(w, 4, [1])
    simple_code(w, 26, [0])
    icount = 2
    w.put(icount - 1, 2)
    # Distances: 2 types, always "second to last", counts 9..12.
    varlen(w, 2)
    simple_code(w, 4, [0])
    simple_code(w, 26, [2])
    dcount = 10
    w.put(dcount - 9, 2)
    w.put(0, 2)  # NPOSTFIX
    w.put(2, 4)  # NDIRECT 2: codes 16, 17 are distances 1, 2
    for mode in (0, 1, 2, 3):
        w.put(mode, 2)
    # Literal context map: 3 trees, RLEMAX 4, IMTF.
    varlen(w, 3)
    cmap = []
    while len(cmap) < 256:
        v = rnd.choice([0, 0, 0, 1, 2])
        cmap += [v] * min(256 - len(cmap), rnd.choice([1, 1, 2, 5, 9, 20, 40]))
    mtf = list(range(256))
    enc = []
    for v in cmap:
        i = mtf.index(v)
        enc.append(i)
        mtf.insert(0, mtf.pop(i))
    w.put(1, 1)
    w.put(3, 4)  # RLEMAX 4
    complex_code_7(w)
    lens7 = [2, 3, 3, 3, 3, 3, 3]
    codes = {}
    code = 0
    for ln in (2, 3):
        for s in range(7):
            if lens7[s] == ln:
                codes[s] = (code, ln)
                code += 1
        code <<= 1

    def put_sym(s):
        c, ln = codes[s]
        w.put(int(format(c, "0%db" % ln)[::-1], 2), ln)

    i = 0
    while i < 256:
        if enc[i] == 0:
            run = 1
            while i + run < 256 and enc[i + run] == 0 and run < 31:
                run += 1
            if run == 1:
                put_sym(0)
            else:
                k = run.bit_length() - 1
                put_sym(k)
                w.put(run - (1 << k), k)
            i += run
        else:
            put_sym(enc[i] + 4)
            i += 1
    w.put(1, 1)  # IMTF
    # Distance context map: 2 trees, no RLE; the two block types pick
    # different trees for copy lengths 2 and 4.
    varlen(w, 2)
    w.put(0, 1)
    simple_code(w, 2, [0, 1])
    for v in (0, 1, 1, 0, 1, 0, 0, 1):
        w.put(v, 1)
    w.put(0, 1)
    # Literal trees, each with four 2-bit codes.
    w.put(1, 2), w.put(3, 2)
    for c in b"abcd":
        w.put(c, 8)
    w.put(0, 1)
    w.put(1, 2), w.put(3, 2)
    for c in b"WXYZ":
        w.put(c, 8)
    w.put(0, 1)
    complex_code_single_cl(w)
    # Insert-and-copy trees: insert 3 copy 4, insert 5 copy 2.
    simple_code(w, 704, [128 + (3 << 3) + 2])
    simple_code(w, 704, [128 + (5 << 3) + 0])
    alpha = 16 + 2 + 48
    simple_code(w, alpha, [16])
    simple_code(w, alpha, [17])
    # Commands.
    itype = 0
    for _ in range(ncmds):
        if icount == 0:
            icount = rnd.randint(1, 4)
            w.put(icount - 1, 2)
            itype ^= 1
        icount -= 1
        for _ in range(3 if itype == 0 else 5):
            if lcount == 0:
                w.put(rnd.choice([0, 1, 2, 3]), 2)
                lcount = rnd.randint(1, 8)
                w.put((lcount - 1) >> 2, 1)
                w.put((lcount - 1) & 3, 2)
            lcount -= 1
            w.put(rnd.getrandbits(2), 2)
        if dcount == 0:
            dcount = rnd.randint(9, 12)
            w.put(dcount - 9, 2)
        dcount -= 1
    open(path, "wb").write(w.data())


def main():
    import random
    d = sys.argv[1]
    craft_dict(os.path.join(d, "craft-dict.br"))
    craft_edge(os.path.join(d, "craft-edge.br"))
    craft_uncompressed(os.path.join(d, "craft-uncompressed.br"))
    craft_blocks(os.path.join(d, "craft-blocks.br"), random.Random(6))


if __name__ == "__main__":
    main()
