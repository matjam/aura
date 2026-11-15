#!/usr/bin/env python3
"""Draws the images the sample pages in tests/page/samples use: the forum
icons and logo, Hacker News' spacer and vote arrow, and the images of
images.html (one or more of every format Aura decodes). Everything is
drawn here from shapes; nothing is downloaded.

Needs ImageMagick (magick) for GIF, PNG, APNG, JPEG, BMP and ICO, and
cwebp and img2webp for WebP. The files are committed; run this only to
change them.

usage: make-sample-images.py"""
import math
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.join(HERE, "samples")
IMG = os.path.join(SAMPLES, "img")
TMP = tempfile.mkdtemp(prefix="aura-sample-images-")


class Canvas:
    """RGBA, straight alpha, floats 0..255."""

    def __init__(self, w, h, bg=(0, 0, 0, 0)):
        self.w, self.h = w, h
        self.px = [list(bg) for _ in range(w * h)]

    def blend(self, x, y, c, cov=1.0):
        if not (0 <= x < self.w and 0 <= y < self.h) or cov <= 0:
            return
        p = self.px[y * self.w + x]
        a = c[3] / 255.0 * min(cov, 1.0)
        da = p[3] / 255.0
        oa = a + da * (1 - a)
        if oa <= 0:
            return
        for i in range(3):
            p[i] = (c[i] * a + p[i] * da * (1 - a)) / oa
        p[3] = oa * 255

    def rect(self, x0, y0, x1, y1, c):
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                self.blend(x, y, c)

    def gradient(self, c0, c1, vertical=True):
        for y in range(self.h):
            for x in range(self.w):
                t = (y / max(1, self.h - 1)) if vertical else (x / max(1, self.w - 1))
                self.px[y * self.w + x] = [c0[i] + (c1[i] - c0[i]) * t for i in range(4)]

    def disc(self, cx, cy, r, c):
        """Anti-aliased by 4x4 supersampling."""
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                n = 0
                for sy in range(4):
                    for sx in range(4):
                        dx = x + (sx + 0.5) / 4 - cx
                        dy = y + (sy + 0.5) / 4 - cy
                        n += dx * dx + dy * dy <= r * r
                self.blend(x, y, c, n / 16)

    def polygon(self, pts, c):
        ys = [p[1] for p in pts]
        for y in range(max(0, int(min(ys))), min(self.h, int(max(ys)) + 1)):
            for x in range(self.w):
                n = 0
                for sy in range(4):
                    for sx in range(4):
                        n += inside(pts, x + (sx + 0.5) / 4, y + (sy + 0.5) / 4)
                self.blend(x, y, c, n / 16)

    def save_pam(self, path):
        with open(path, "wb") as f:
            f.write(b"P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (self.w, self.h))
            f.write(bytes(max(0, min(255, int(v + 0.5))) for p in self.px for v in p))
        return path


def inside(pts, x, y):
    c = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


def add_exif_orientation(path, orientation):
    """Inserts an APP1 Exif segment holding only the Orientation tag
    (big-endian TIFF, one IFD entry) after the JPEG's SOI marker."""
    tiff = b"MM\x00\x2a\x00\x00\x00\x08" + b"\x00\x01" + b"\x01\x12\x00\x03\x00\x00\x00\x01"
    tiff += bytes([0, orientation, 0, 0]) + b"\x00\x00\x00\x00"
    body = b"Exif\x00\x00" + tiff
    seg = b"\xff\xe1" + (len(body) + 2).to_bytes(2, "big") + body
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"\xff\xd8"
    with open(path, "wb") as f:
        f.write(data[:2] + seg + data[2:])


def tmp(name):
    return os.path.join(TMP, name)


def run(*args):
    subprocess.run([str(a) for a in args], check=True)


def save(canvas, out, *opts, fmt=""):
    """Writes canvas to out through magick; fmt is an output format prefix
    such as PNG8 or BMP3."""
    src = canvas.save_pam(tmp(os.path.basename(out) + ".pam"))
    run("magick", src, *opts, (fmt + ":" if fmt else "") + out)
    return src


# ---- Scenes ----


def landscape(w, h, t=0.0):
    """Sky, sun, hills and a lake: smooth gradients and edges for JPEG."""
    c = Canvas(w, h)
    c.gradient((70, 130, 220, 255), (250, 200, 150, 255))
    c.disc(w * 0.72, h * 0.3, h * 0.12, (255, 235, 120, 255))
    for k, (col, base, amp, freq) in enumerate(
        [((90, 120, 80, 255), 0.55, 0.12, 2.1), ((60, 100, 50, 255), 0.68, 0.08, 3.7), ((40, 70, 35, 255), 0.8, 0.05, 6.3)]
    ):
        pts = [(0, h)]
        for i in range(0, 41):
            x = w * i / 40
            pts.append((x, h * (base - amp * math.sin(i / 40 * math.pi * freq + k + t))))
        pts.append((w, h))
        c.polygon(pts, col)
    for y in range(int(h * 0.86), h):
        for x in range(w):
            p = c.px[y * w + x]
            v = 0.5 + 0.5 * math.sin(x * 0.15 + y * 0.9)
            c.px[y * w + x] = [40 + 30 * v, 90 + 40 * v, 170 + 40 * v, 255]
    return c


def badge(w, h, col, label_pts):
    c = Canvas(w, h)
    c.disc(w / 2, h / 2, min(w, h) / 2 - 1, col)
    c.polygon(label_pts, (255, 255, 255, 255))
    return c


def spinner(size, frame, frames, col):
    c = Canvas(size, size)
    for k in range(8):
        ang = 2 * math.pi * k / 8
        a = 255 * (0.25 + 0.75 * ((k - frame) % frames == 0 and 1 or ((frames - (k - frame) % frames) / frames) ** 2))
        c.disc(size / 2 + math.cos(ang) * size * 0.33, size / 2 + math.sin(ang) * size * 0.33, size * 0.09,
               (col[0], col[1], col[2], a))
    return c


def bouncing(w, h, frame, frames):
    c = Canvas(w, h, (245, 245, 235, 255))
    c.rect(0, h - 6, w, h, (120, 90, 60, 255))
    t = frame / frames
    y = h - 6 - 10 - abs(math.sin(t * math.pi)) * (h - 30)
    c.disc(10 + t * (w - 20), y, 10, (220, 60, 50, 255))
    return c


# ---- Pages' images ----


def forum_images():
    logo = Canvas(200, 91)
    logo.gradient((30, 60, 120, 255), (90, 150, 210, 255), vertical=False)
    logo.disc(45, 45, 32, (255, 255, 255, 230))
    logo.disc(45, 45, 22, (30, 60, 120, 255))
    logo.polygon([(90, 60), (110, 25), (130, 60), (122, 60), (110, 38), (98, 60)], (255, 255, 255, 255))
    logo.rect(140, 25, 148, 60, (255, 255, 255, 255))
    logo.rect(155, 25, 185, 33, (255, 255, 255, 255))
    logo.rect(155, 39, 180, 46, (255, 255, 255, 255))
    logo.rect(155, 25, 163, 60, (255, 255, 255, 255))
    save(logo, os.path.join(SAMPLES, "logo_phpBB.gif"))
    icons = {
        "icon_mini_faq.gif": (60, 120, 200),
        "icon_mini_search.gif": (200, 140, 40),
        "icon_mini_members.gif": (60, 160, 90),
        "icon_mini_groups.gif": (150, 80, 170),
        "icon_mini_profile.gif": (40, 140, 160),
        "icon_mini_login.gif": (190, 60, 60),
    }
    for name, col in icons.items():
        c = Canvas(12, 13)
        c.disc(6, 6.5, 5.5, col + (255,))
        c.disc(6, 6.5, 2, (255, 255, 255, 255))
        save(c, os.path.join(SAMPLES, name))
    for name, col, lock in [("folder.gif", (230, 190, 90), False), ("folder_new.gif", (240, 120, 50), False),
                            ("folder_lock.gif", (160, 160, 160), True)]:
        c = Canvas(19, 18)
        c.rect(1, 3, 8, 6, col + (255,))
        c.rect(1, 5, 18, 16, col + (255,))
        c.rect(1, 15, 18, 16, (110, 80, 30, 255))
        if lock:
            c.rect(7, 8, 12, 13, (70, 70, 70, 255))
        save(c, os.path.join(SAMPLES, name))
    for name, col in [("folder_big.gif", (230, 190, 90)), ("folder_new_big.gif", (240, 120, 50)),
                      ("whosonline.gif", (100, 160, 220))]:
        c = Canvas(46, 25)
        c.rect(10, 3, 22, 7, col + (255,))
        c.rect(10, 6, 36, 23, col + (255,))
        c.rect(10, 21, 36, 23, (110, 80, 30, 255))
        save(c, os.path.join(SAMPLES, name))
    c = Canvas(18, 9)
    c.polygon([(2, 1), (16, 4.5), (2, 8)], (120, 120, 120, 255))
    save(c, os.path.join(SAMPLES, "icon_latest_reply.gif"))


def hn_images():
    save(Canvas(1, 1), os.path.join(SAMPLES, "s.gif"))
    c = Canvas(10, 10)
    c.polygon([(5, 1), (9.5, 9), (0.5, 9)], (154, 154, 154, 255))
    save(c, os.path.join(SAMPLES, "grayarrow.gif"))


def images_page():
    os.makedirs(IMG, exist_ok=True)
    land = landscape(240, 160)
    save(land, os.path.join(IMG, "landscape.jpg"), "-quality", "85", "-sampling-factor", "2x2")
    save(land, os.path.join(IMG, "progressive.jpg"), "-quality", "80", "-interlace", "JPEG")
    tall = landscape(160, 240)
    # Stored rotated 90 degrees anticlockwise with EXIF orientation 6
    # (rotate 90 clockwise to display): shows upright when honoured.
    rotated = os.path.join(IMG, "exif-rotated.jpg")
    save(tall, rotated, "-rotate", "-90", "-quality", "85", "-strip")
    add_exif_orientation(rotated, 6)
    save(landscape(240, 160), os.path.join(IMG, "grey.jpg"), "-colorspace", "Gray", "-quality", "85")
    big = landscape(960, 640)
    save(big, os.path.join(IMG, "large.jpg"), "-quality", "85")
    alpha = Canvas(120, 120)
    alpha.disc(60, 60, 55, (40, 120, 220, 200))
    alpha.disc(45, 45, 25, (255, 255, 255, 160))
    alpha.polygon([(60, 20), (100, 95), (20, 95)], (240, 200, 40, 180))
    save(alpha, os.path.join(IMG, "alpha.png"))
    save(alpha, os.path.join(IMG, "interlaced.png"), "-interlace", "PNG")
    save(land, os.path.join(IMG, "palette.png"), "-colors", "64", fmt="PNG8")
    save(alpha, tmp("alpha.png"))
    save(land, tmp("land.png"))
    run("cwebp", "-quiet", "-lossless", tmp("alpha.png"), "-o", os.path.join(IMG, "lossless.webp"))
    run("cwebp", "-quiet", "-q", "80", tmp("land.png"), "-o", os.path.join(IMG, "lossy.webp"))
    run("cwebp", "-quiet", "-q", "80", "-alpha_q", "100", tmp("alpha.png"), "-o", os.path.join(IMG, "lossy-alpha.webp"))
    # Animations: a spinner (GIF, APNG, WebP) and a bouncing ball (GIF).
    frames = []
    for i in range(8):
        frames.append(spinner(48, i, 8, (50, 90, 200)).save_pam(tmp("spin%d.pam" % i)))
    run("magick", "-delay", "10", "-dispose", "background", *frames, "-loop", "0", os.path.join(IMG, "spinner.gif"))
    run("magick", "-delay", "10", "-dispose", "background", *frames, "-loop", "0",
        "apng:" + os.path.join(IMG, "spinner.png"))
    pngs = []
    for i, f in enumerate(frames):
        pngs.append(tmp("spin%d.png" % i))
        run("magick", f, pngs[-1])
    args = ["img2webp", "-loop", "0", "-lossless", "-d", "100"] + pngs + ["-o", os.path.join(IMG, "spinner.webp")]
    run(*args)
    balls = [bouncing(120, 60, i, 12).save_pam(tmp("ball%d.pam" % i)) for i in range(12)]
    run("magick", "-delay", "8", *balls, "-loop", "0", "-layers", "Optimize", os.path.join(IMG, "bounce.gif"))
    # BMP, ICO, and the small images for CSS and forms.
    save(land, os.path.join(IMG, "landscape.bmp"), "-type", "TrueColor", fmt="BMP3")
    icon = [badge(s, s, (200, 60, 60, 255), [(s * 0.3, s * 0.25), (s * 0.75, s * 0.5), (s * 0.3, s * 0.75)])
            for s in (16, 32, 48)]
    run("magick", *[c.save_pam(tmp("icon%d.pam" % c.w)) for c in icon], os.path.join(IMG, "favicon.ico"))
    tile = Canvas(16, 16, (250, 246, 230, 255))
    tile.rect(0, 0, 8, 8, (238, 230, 205, 255))
    tile.rect(8, 8, 16, 16, (238, 230, 205, 255))
    save(tile, os.path.join(IMG, "tile.png"))
    bullet = Canvas(9, 9)
    bullet.disc(4.5, 4.5, 4, (60, 150, 80, 255))
    save(bullet, os.path.join(IMG, "bullet.gif"))
    button = Canvas(88, 31)
    button.gradient((90, 170, 90, 255), (40, 110, 40, 255))
    button.polygon([(30, 8), (58, 15.5), (30, 23)], (255, 255, 255, 255))
    save(button, os.path.join(IMG, "button.png"))
    pix = Canvas(8, 8, (255, 255, 255, 255))
    for y, row in enumerate(["..XXXX..", ".X....X.", "X.X..X.X", "X......X", "X.X..X.X", "X..XX..X", ".X....X.",
                             "..XXXX.."]):
        for x, ch in enumerate(row):
            if ch == "X":
                pix.rect(x, y, x + 1, y + 1, (30, 30, 30, 255))
    save(pix, os.path.join(IMG, "pixel.png"))


def main():
    forum_images()
    hn_images()
    images_page()
    return 0


if __name__ == "__main__":
    sys.exit(main())
