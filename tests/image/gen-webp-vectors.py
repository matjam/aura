#!/usr/bin/env python3
"""Generates the WebP test vectors in tests/image/vectors/webp.

Every image is synthesized here (gradients, edges, noise, alpha), encoded
with the host's libwebp tools (cwebp, webpmux, img2webp) and decoded with
dwebp -pam for the reference. Token partitions (2, 4, 8) are not exposed
by cwebp, so a small C helper is compiled against the host's libwebp
(cc, /opt/homebrew or /usr/local) to make those; they are skipped when
that fails.

Still images: NAME.webp and NAME.pam (dwebp's RGBA output, fancy
upsampling). Animations: NAME.webp, NAME.anim (a text file: "loop N",
then one "frame DELAY TOLERANCE" line per frame) and NAME.fK.pam, the
canvas after frame K. The canvases are composited here from the frames
(webpmux -info gives each frame's offset, duration, blending and
disposal; webpmux -get frame extracts it; dwebp decodes it) on a
transparent canvas, as browsers composite. From the first frame that
blends a partly transparent pixel over another on, frames get tolerance
2 (the decoder blends in premultiplied form); the others are exact.

To regenerate, run from the repository root:
    python3 tests/image/gen-webp-vectors.py
To make the large speed-test images (not committed) instead:
    python3 tests/image/gen-webp-vectors.py --big build/webp
To make references for the libwebp test data, if downloaded into
build/webp-testdata (see webp.cmake):
    python3 tests/image/gen-webp-vectors.py --testdata build/webp-testdata
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "vectors", "webp")


def rng(seed):
    s = (seed * 2654435761) % 2**32 or 1
    while True:
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        yield s


def pattern(w, h, seed=1, alpha=False, noise=16):
    """Gradients, a ring, a checker of inverted colours, a ramp and noise;
    with alpha: a radial alpha ramp, a transparent corner, an opaque band."""
    r_ = rng(seed)
    px = bytearray()
    for y in range(h):
        for x in range(w):
            n = (next(r_) % (2 * noise + 1)) - noise if noise else 0
            r = x * 255 // max(1, w - 1)
            g = y * 255 // max(1, h - 1)
            dx, dy = x - w / 2, y - h / 2
            d2 = dx * dx + dy * dy
            b = 230 if (w * h / 16) < d2 < (w * h / 7) else 40
            if (x // 6 + y // 6) % 2 and x > w * 2 // 3:
                r, g, b = 255 - r, 255 - g, 255 - b
            if y > h * 3 // 4 and x < w // 3:
                r = g = b = (x * 37) & 255
            rgb = [max(0, min(255, v + n)) for v in (r, g, b)]
            if alpha:
                a = max(0, min(255, int(255 - (d2 ** 0.5) * 400 / max(w, h))))
                if x < w // 4 and y < h // 4:
                    a = 0
                if h // 2 - 2 <= y <= h // 2 + 2:
                    a = 255
                rgb.append(a)
            px += bytes(rgb)
    return px


def palette_image(w, h, ncolors, seed=3):
    r_ = rng(seed)
    pal = [(next(r_) & 255, next(r_) & 255, next(r_) & 255, 255 if i % 3 else 128) for i in range(ncolors)]
    px = bytearray()
    for y in range(h):
        for x in range(w):
            i = ((x // 3) * 7 + (y // 2) * 3 + (x * y) % 5) % ncolors
            px += bytes(pal[i])
    return px


def sprite(w, h, seed, semi=False):
    """A shape with binary alpha (or a soft edge with semi)."""
    r_ = rng(seed)
    px = bytearray()
    cx, cy = w / 2, h / 2
    for y in range(h):
        for x in range(w):
            d = ((x - cx) ** 2 / (cx * cx) + (y - cy) ** 2 / (cy * cy)) ** 0.5
            col = (next(r_) & 63) + (x * 190 // max(1, w - 1), 200 - y * 3 % 200, (seed * 50) % 256)[(x + y) % 3]
            rgb = [min(255, col), (seed * 70 + x * 5) & 255, (y * 9 + seed * 30) & 255]
            if semi:
                a = 0 if d > 1 else int(255 * (1 - d) ** 0.5)
            else:
                a = 255 if d < 0.9 else 0
            px += bytes(rgb + [a])
    return px


def write_pam(path, w, h, px, depth):
    tup = {1: "GRAYSCALE", 3: "RGB", 4: "RGB_ALPHA"}[depth]
    with open(path, "wb") as f:
        f.write(b"P7\nWIDTH %d\nHEIGHT %d\nDEPTH %d\nMAXVAL 255\nTUPLTYPE %s\nENDHDR\n" % (w, h, depth, tup.encode()))
        f.write(px)


def read_pam(path):
    data = open(path, "rb").read()
    end = data.index(b"ENDHDR\n") + 7
    hdr = dict(l.split(b" ", 1) for l in data[:end].split(b"\n")[1:-2])
    w, h, d = int(hdr[b"WIDTH"]), int(hdr[b"HEIGHT"]), int(hdr[b"DEPTH"])
    px = data[end:]
    if d == 3:
        px = b"".join(px[i:i + 3] + b"\xff" for i in range(0, len(px), 3))
    return w, h, bytearray(px)


def run(*args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Gen:
    def __init__(self, out):
        self.out = out
        self.tmp = tempfile.mkdtemp()
        self.helper = None

    def src(self, name, w, h, px, depth):
        p = os.path.join(self.tmp, name + (".pam" if depth == 4 else ".ppm"))
        if depth == 4:
            write_pam(p, w, h, px, 4)
        else:
            with open(p, "wb") as f:
                f.write(b"P6\n%d %d\n255\n" % (w, h))
                f.write(px)
        return p

    def ref(self, name):
        webp = os.path.join(self.out, name + ".webp")
        run("dwebp", webp, "-pam", "-o", os.path.join(self.out, name + ".pam"))

    def still(self, name, w, h, px, depth, *opts):
        s = self.src(name, w, h, px, depth)
        run("cwebp", "-quiet", *opts, s, "-o", os.path.join(self.out, name + ".webp"))
        self.ref(name)

    def build_helper(self):
        """A libwebp program that sets the token partition count."""
        c = os.path.join(self.tmp, "parts.c")
        with open(c, "w") as f:
            f.write(r"""
#include <stdio.h>
#include <stdlib.h>
#include <webp/encode.h>
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); int w, h, m;
    if (!f || fscanf(f, "P6 %d %d %d", &w, &h, &m) != 3) return 1;
    fgetc(f);
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (fread(rgb, 3, (size_t)w * h, f) != (size_t)w * h) return 1;
    WebPConfig cfg; WebPPicture pic; WebPMemoryWriter wr;
    if (!WebPConfigInit(&cfg) || !WebPPictureInit(&pic)) return 1;
    cfg.quality = (float)atof(argv[3]); cfg.partitions = atoi(argv[4]);
    cfg.filter_type = atoi(argv[5]); cfg.segments = atoi(argv[6]); cfg.method = 4;
    pic.width = w; pic.height = h;
    if (!WebPPictureImportRGB(&pic, rgb, w * 3)) return 1;
    WebPMemoryWriterInit(&wr); pic.writer = WebPMemoryWrite; pic.custom_ptr = &wr;
    if (!WebPEncode(&cfg, &pic)) return 1;
    FILE *o = fopen(argv[2], "wb"); fwrite(wr.mem, 1, wr.size, o); fclose(o);
    return 0;
}
""")
        exe = os.path.join(self.tmp, "parts")
        for prefix in ("/opt/homebrew", "/usr/local", "/usr"):
            try:
                run("cc", "-o", exe, c, "-I" + prefix + "/include", "-L" + prefix + "/lib", "-lwebp")
                self.helper = exe
                return True
            except (subprocess.CalledProcessError, FileNotFoundError):
                pass
        print("note: no libwebp development files; partition vectors not made")
        return False

    def partitions(self, name, w, h, px, q, parts, ftype, segments):
        s = self.src(name, w, h, px, 3)
        run(self.helper, s, os.path.join(self.out, name + ".webp"), str(q), str(parts), str(ftype), str(segments))
        self.ref(name)

    def anim(self, name, frames, loop, extra=()):
        """frames: (w, h, px, depth, cwebp options, webpmux frame option)."""
        args = []
        for i, (w, h, px, depth, opts, fopt) in enumerate(frames):
            s = self.src("%s_%d" % (name, i), w, h, px, depth)
            f = os.path.join(self.tmp, "%s_%d.webp" % (name, i))
            run("cwebp", "-quiet", *opts, s, "-o", f)
            args += ["-frame", f, fopt]
        out = os.path.join(self.out, name + ".webp")
        run("webpmux", *args, "-loop", str(loop), *extra, "-o", out)
        self.anim_refs(name, out)

    def anim_refs(self, name, path, outdir=None):
        outdir = outdir or self.out
        info = subprocess.run(["webpmux", "-info", path], check=True, capture_output=True, text=True).stdout
        cw, ch = map(int, re.search(r"Canvas size: (\d+) x (\d+)", info).group(1, 2))
        loop = int(re.search(r"Loop Count : (\d+)", info).group(1))
        rows = re.findall(r"^\s*(\d+):\s+(\d+)\s+(\d+)\s+(\w+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\w+)\s+(\w+)", info, re.M)
        canvas = bytearray(cw * ch * 4)
        lines = ["loop %d" % loop]
        prev = None
        tol = 0  # stays 2 once a blended pixel is on the canvas
        for (no, fw, fh, _, fx, fy, dur, dispose, blend) in rows:
            fw, fh, fx, fy = int(fw), int(fh), int(fx), int(fy)
            if prev:
                px_, py_, pw_, ph_ = prev
                for y in range(py_, py_ + ph_):
                    canvas[(y * cw + px_) * 4:(y * cw + px_ + pw_) * 4] = bytes(pw_ * 4)
            fpath = os.path.join(self.tmp, "frame.webp")
            run("webpmux", "-get", "frame", no, path, "-o", fpath)
            run("dwebp", fpath, "-pam", "-o", fpath + ".pam")
            w_, h_, src = read_pam(fpath + ".pam")
            assert (w_, h_) == (fw, fh)
            for y in range(fh):
                for x in range(fw):
                    s = src[(y * fw + x) * 4:(y * fw + x) * 4 + 4]
                    o = ((fy + y) * cw + fx + x) * 4
                    d = canvas[o:o + 4]
                    if blend == "yes" and s[3] < 255:
                        if s[3] == 0:
                            continue
                        if d[3] == 0:
                            canvas[o:o + 4] = s
                            continue
                        # Source over, non-premultiplied result.
                        sa, da = s[3] / 255, d[3] / 255
                        oa = sa + da * (1 - sa)
                        canvas[o:o + 4] = bytes(
                            [round((s[c] * sa + d[c] * da * (1 - sa)) / oa) for c in range(3)] + [round(oa * 255)])
                        tol = 2
                    else:
                        canvas[o:o + 4] = s
            write_pam(os.path.join(outdir, "%s.f%s.pam" % (name, int(no) - 1)), cw, ch, canvas, 4)
            lines.append("frame %s %d" % (dur, tol))
            prev = (fx, fy, fw, fh) if dispose == "background" else None
        with open(os.path.join(outdir, name + ".anim"), "w") as f:
            f.write("\n".join(lines) + "\n")


def make_vectors(out):
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(out, "*")):
        os.remove(f)
    g = Gen(out)
    W, H = 48, 36
    rgb = pattern(W, H, 1)
    rgba = pattern(W, H, 2, alpha=True)

    # Lossy (VP8).
    g.still("l_q90", W, H, rgb, 3, "-q", "90")
    g.still("l_q50_odd", 33, 17, pattern(33, 17, 4), 3, "-q", "50")
    g.still("l_q5", 96, 64, pattern(96, 64, 5), 3, "-q", "5")
    g.still("l_q100", 40, 40, pattern(40, 40, 6), 3, "-q", "100")
    g.still("l_1x1", 1, 1, b"\x80\x40\xc0", 3)
    g.still("l_3x7", 3, 7, pattern(3, 7, 7), 3)
    g.still("l_nofilter", W, H, rgb, 3, "-f", "0", "-q", "70")
    g.still("l_strong", W, H, rgb, 3, "-f", "100", "-sharpness", "0", "-q", "40")
    g.still("l_sharp7", W, H, rgb, 3, "-f", "80", "-sharpness", "7", "-q", "40")
    g.still("l_simple", 80, 64, pattern(80, 64, 8), 3, "-nostrong", "-f", "70", "-q", "30")
    g.still("l_seg1", W, H, rgb, 3, "-segments", "1", "-q", "60")
    g.still("l_sns100", 80, 64, pattern(80, 64, 9, noise=40), 3, "-sns", "100", "-segments", "4", "-q", "60")
    g.still("l_sns0", W, H, rgb, 3, "-sns", "0", "-q", "60")
    g.still("l_sharpyuv", W, H, rgb, 3, "-sharp_yuv", "-q", "80")
    g.still("l_m0", W, H, rgb, 3, "-m", "0", "-q", "75")
    g.still("l_m6", W, H, rgb, 3, "-m", "6", "-q", "75", "-pass", "6")
    g.still("l_smooth", 72, 56, pattern(72, 56, 10, noise=0), 3, "-q", "85")
    g.still("l_120x88", 120, 88, pattern(120, 88, 11), 3, "-q", "60")
    g.still("l_plimit", 64, 64, pattern(64, 64, 12, noise=60), 3, "-q", "95", "-partition_limit", "100")
    # Lossy with alpha (ALPH): compression, filters, quantized alpha.
    g.still("la_default", W, H, rgba, 4, "-q", "80")
    g.still("la_raw", W, H, rgba, 4, "-q", "80", "-alpha_method", "0")
    g.still("la_fnone", W, H, rgba, 4, "-alpha_filter", "none")
    g.still("la_ffast", W, H, rgba, 4, "-alpha_filter", "fast")
    g.still("la_fbest", 50, 30, pattern(50, 30, 13, alpha=True), 4, "-alpha_filter", "best")
    g.still("la_q50", W, H, rgba, 4, "-alpha_q", "50")
    g.still("la_raw_odd", 17, 9, pattern(17, 9, 14, alpha=True), 4, "-alpha_method", "0", "-alpha_filter", "best")
    if g.build_helper():
        big = pattern(96, 64, 15)
        g.partitions("l_parts2", 96, 64, big, 70, 1, 1, 4)
        g.partitions("l_parts4", 96, 64, big, 70, 2, 0, 2)
        g.partitions("l_parts8", 40, 144, pattern(40, 144, 16), 50, 3, 1, 4)

    # Lossless (VP8L): effort levels give different transforms and caches.
    for z in (0, 3, 6, 9):
        g.still("ll_z%d" % z, W, H, rgba, 4, "-lossless", "-z", str(z))
    g.still("ll_exact", W, H, rgba, 4, "-lossless", "-exact")
    g.still("ll_rgb_noise", 48, 40, pattern(48, 40, 17, noise=100), 3, "-lossless", "-z", "5")
    g.still("ll_smooth", 100, 70, pattern(100, 70, 18, noise=0), 3, "-lossless", "-z", "9")
    for n in (2, 4, 16, 200):
        g.still("ll_pal%d" % n, 45, 31, palette_image(45, 31, n), 4, "-lossless")
    g.still("ll_1x1", 1, 1, b"\x10\x20\x30\x40", 4, "-lossless")
    g.still("ll_3x7", 3, 7, pattern(3, 7, 19, alpha=True), 4, "-lossless", "-exact")
    g.still("ll_near", W, H, rgba, 4, "-lossless", "-near_lossless", "40")
    g.still("ll_120x80", 120, 80, pattern(120, 80, 20, noise=3), 3, "-lossless", "-z", "6")
    g.still("ll_m0q0", W, H, rgb, 3, "-lossless", "-m", "0", "-q", "0")

    # Extended files: VP8X with ICC, EXIF and XMP chunks to skip.
    meta = os.path.join(g.tmp, "meta.bin")
    with open(meta, "wb") as f:
        f.write(b"Exif\0\0" + bytes(range(37)))
    for name, opts, depth, px in (("x_lossy_meta", ("-q", "70"), 3, rgb), ("x_lossless_meta", ("-lossless",), 4, rgba),
                                  ("x_alpha_meta", ("-q", "70"), 4, rgba)):
        s = g.src(name, W, H, px, depth)
        t = os.path.join(g.tmp, name + ".webp")
        run("cwebp", "-quiet", *opts, s, "-o", t)
        t2 = t + ".2.webp"
        run("webpmux", "-set", "icc", meta, t, "-o", t2)
        run("webpmux", "-set", "exif", meta, t2, "-o", t)
        run("webpmux", "-set", "xmp", meta, t, "-o", os.path.join(out, name + ".webp"))
        g.ref(name)

    # Animations.
    bg = pattern(64, 48, 21)
    g.anim("a_blend", [
        (64, 48, bg, 3, ("-lossless",), "+100+0+0+0-b"),
        (32, 24, sprite(32, 24, 1), 4, ("-lossless",), "+50+8+6+0+b"),
        (20, 20, sprite(20, 20, 2), 4, ("-lossless", "-exact"), "+70+30+20+1-b"),
        (24, 16, sprite(24, 16, 3), 4, ("-q", "80"), "+200+2+30+0+b"),
        (30, 18, sprite(30, 18, 4), 4, ("-lossless",), "+0+34+28+1+b"),
    ], loop=3)
    g.anim("a_semi", [
        (40, 40, pattern(40, 40, 22), 3, ("-q", "90"), "+80+0+0+0-b"),
        (30, 30, sprite(30, 30, 5, semi=True), 4, ("-lossless", "-exact"), "+60+4+6+0+b"),
        (20, 26, sprite(20, 26, 6, semi=True), 4, ("-q", "75"), "+40+16+10+1+b"),
        (16, 16, sprite(16, 16, 7, semi=True), 4, ("-lossless",), "+30+20+20+0-b"),
    ], loop=0)
    g.anim("a_dispose", [
        (24, 24, sprite(24, 24, 8), 4, ("-lossless",), "+100+0+0+1+b"),
        (24, 24, sprite(24, 24, 9), 4, ("-lossless",), "+100+16+12+1+b"),
        (24, 24, sprite(24, 24, 10), 4, ("-q", "60"), "+100+30+22+1-b"),
    ], loop=1)
    # An encoder-built animation (sub-rectangles, mixed lossy/lossless).
    frames = []
    for i in range(4):
        px = bytearray(pattern(48, 36, 30, alpha=True, noise=0))
        for y in range(8 + i * 4, 20 + i * 4):
            for x in range(10 + i * 6, 22 + i * 6):
                px[(y * 48 + x) * 4:(y * 48 + x) * 4 + 4] = bytes([255, i * 60, 0, 255])
        frames.append(g.src("enc%d" % i, 48, 36, px, 4))
    out_enc = os.path.join(out, "a_img2webp.webp")
    run("img2webp", "-loop", "2", "-mixed", "-d", "40", frames[0], "-d", "60", frames[1], "-lossy", "-q", "70",
        "-d", "80", frames[2], "-lossless", "-d", "90", frames[3], "-o", out_enc)
    g.anim_refs("a_img2webp", out_enc)
    shutil.rmtree(g.tmp)


def make_big(out):
    """Large images for the speed test (not committed)."""
    os.makedirs(out, exist_ok=True)
    g = Gen(out)
    w, h = 1024, 768
    s = g.src("big", w, h, pattern(w, h, 40, noise=8), 3)
    run("cwebp", "-quiet", "-q", "80", s, "-o", os.path.join(out, "big_lossy.webp"))
    run("cwebp", "-quiet", "-lossless", s, "-o", os.path.join(out, "big_lossless.webp"))
    shutil.rmtree(g.tmp)


def make_testdata_refs(d):
    """References (dwebp -pam) for downloaded conformance files."""
    ref = os.path.join(d, "ref")
    os.makedirs(ref, exist_ok=True)
    g = Gen(ref)
    for f in sorted(glob.glob(os.path.join(d, "*.webp"))):
        name = os.path.splitext(os.path.basename(f))[0]
        info = subprocess.run(["webpmux", "-info", f], capture_output=True, text=True)
        try:
            if "animation" in info.stdout:
                g.anim_refs(name, f, ref)
            else:
                run("dwebp", f, "-pam", "-o", os.path.join(ref, name + ".pam"))
        except (subprocess.CalledProcessError, AttributeError, AssertionError):
            print("no reference for", name)
    shutil.rmtree(g.tmp)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--big":
        make_big(sys.argv[2])
    elif len(sys.argv) == 3 and sys.argv[1] == "--testdata":
        make_testdata_refs(sys.argv[2])
    else:
        make_vectors(OUT)
