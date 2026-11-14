# SPDX-License-Identifier: GPL-3.0-only
"""FMLarynx's splash art: a silly open throat (wobbly lips, crooked teeth, a swinging uvula, the tongue out) under
the name in Terminus (fm1-base's assets/fonts, as the screen's own letters), 240x240 -> assets/splash.png. The
version is not in the picture: app_splash writes it under the throat at boot (rows 226..237 stay dark for it).
usage: python -I tools/splash_art.py assets/splash.png [PREVIEW_x3.png]"""
import math
import sys
from PIL import Image, ImageDraw, ImageFilter

S = 4                                       # supersampling for the shapes (the letters are drawn at 1x, crisp)
W = 240 * S
BG = (28, 6, 12)                            # the app's C_BG
FLESH = (246, 205, 196)                     # C_KEY: teeth
LIT = (232, 64, 88)                         # C_LIT: the lips, the name's colour on the screen
DARK = (60, 8, 22)
RINGS = [(214, 112, 122), (198, 88, 102), (176, 64, 82), (148, 44, 64), (114, 28, 46), (80, 14, 30), (46, 5, 16)]
FONT = "base/assets/fonts/ter-u24b.bdf"


def s(v):
    return v * S


def wobble(cx, cy, rx, ry, k, phase=0.0, n=180):
    """an ellipse whose radius wobbles: k the amount"""
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        f = 1 + k * (0.6 * math.sin(5 * t + phase) + 0.4 * math.sin(3 * t + 1.7 + phase))
        pts.append((s(cx + rx * f * math.cos(t)), s(cy + ry * f * math.sin(t))))
    return pts


def tooth(d, x, y, w, h, angle, down=True, colour=FLESH):
    """a rounded tooth hanging from (x, y) (down) or standing on it (up), turned by angle degrees"""
    c, sn = math.cos(math.radians(angle)), math.sin(math.radians(angle))
    sign = 1 if down else -1
    pts = []
    for i in range(24):                     # flat root, round tip
        t = math.pi * i / 23
        px = -w / 2 * math.cos(t)
        py = sign * (h - w / 2 + w / 2 * math.sin(t))
        pts.append((px, py))
    pts = [(-w / 2, 0)] + pts + [(w / 2, 0)]
    d.polygon([(s(x + px * c - py * sn), s(y + px * sn + py * c)) for px, py in pts], fill=colour, outline=DARK,
              width=S)


def art():
    im = Image.new("RGB", (W, W), BG)
    d = ImageDraw.Draw(im)
    cx, cy = 120, 146
    # the lips, then the throat inside: rings deeper towards the back, a little above the middle
    d.polygon(wobble(cx, cy, 104, 74, 0.045), fill=DARK)
    d.polygon(wobble(cx, cy, 100, 70, 0.045), fill=LIT)
    d.polygon(wobble(cx, cy + 1, 88, 59, 0.05, 0.4), fill=DARK)
    for i, col in enumerate(RINGS):
        f = 1 - i / len(RINGS)
        d.polygon(wobble(cx + 2 * i, cy - 4 - 2 * i, 86 * f, 57 * f, 0.05 + 0.01 * i, 0.4 + 0.5 * i), fill=col)
    # the uvula: a drop swinging to the right, with two swing lines
    u = Image.new("L", (W, W), 0)
    ud = ImageDraw.Draw(u)
    ud.ellipse([s(115), s(106), s(135), s(130)], fill=255)
    ud.polygon([(s(114), s(90)), (s(134), s(90)), (s(133), s(114)), (s(117), s(114))], fill=255)
    u = u.rotate(-14, center=(s(124), s(92)), resample=Image.BICUBIC)
    im.paste((236, 112, 132), (0, 0), u)
    d.ellipse([s(127), s(116), s(131), s(121)], fill=(250, 190, 200))
    for r in (24, 31):
        d.arc([s(124 - r), s(92 - r + 18), s(124 + r), s(92 + r + 18)], 20, 55, fill=(250, 170, 182), width=S * 2)
    # the teeth: crooked, one missing, one gold
    top = [(46, 92, 12, 13, -10), (60, 85, 13, 15, -5), (75, 81, 14, 17, 4), (91, 79, 15, 18, -3),
           (126, 79, 15, 17, 6), (142, 80, 14, 19, -8), (158, 83, 13, 14, 3), (173, 88, 12, 12, 12),
           (187, 95, 10, 10, -6)]
    for k, (x, y, w, h, a) in enumerate(top):
        tooth(d, x, y, w, h, a, True, (236, 190, 70) if k == 5 else FLESH)
    for x, y, w, h, a in [(66, 202, 12, 11, 8), (82, 206, 13, 13, -6), (165, 204, 12, 12, 10), (179, 199, 11, 10, -4)]:
        tooth(d, x, y, w, h, a, False)
    # the tongue, out over the lower lip and drooping to the right
    t = Image.new("L", (W, W), 0)
    td = ImageDraw.Draw(t)
    td.ellipse([s(76), s(166), s(168), s(204)], fill=255)      # its back, a hump inside the mouth
    td.ellipse([s(98), s(176), s(150), s(224)], fill=255)      # and the part hanging out
    td.rectangle([s(98), s(186), s(150), s(200)], fill=255)
    t = t.rotate(-8, center=(s(122), s(186)), resample=Image.BICUBIC)
    edge = t.filter(ImageFilter.MaxFilter(2 * S + 1))
    im.paste(DARK, (0, 0), edge)
    im.paste((236, 118, 140), (0, 0), t)
    d.line([(s(122), s(176)), (s(125), s(194)), (s(123), s(212))], fill=(196, 72, 98), width=S * 2)
    d.ellipse([s(100), s(178), s(112), s(186)], fill=(250, 170, 186))
    # a drop of spit flying off
    d.ellipse([s(198), s(196), s(206), s(206)], fill=(220, 230, 240), outline=DARK, width=S)
    d.polygon([(s(198), s(200)), (s(194), s(192)), (s(203), s(197))], fill=(220, 230, 240))
    return im.resize((240, 240), Image.LANCZOS)


def bdf(path):
    """the font's glyphs: char -> (dwidth, w, h, xoff, yoff, rows); and its ascent"""
    glyphs, ascent, cur = {}, 0, None
    with open(path, encoding="latin-1") as f:
        lines = iter(f.read().splitlines())
    for line in lines:
        p = line.split()
        if not p:
            continue
        if p[0] == "FONT_ASCENT":
            ascent = int(p[1])
        elif p[0] == "ENCODING":
            cur = {"code": int(p[1])}
        elif p[0] == "DWIDTH" and cur is not None:
            cur["dw"] = int(p[1])
        elif p[0] == "BBX" and cur is not None:
            cur["bbx"] = tuple(int(v) for v in p[1:5])
        elif p[0] == "BITMAP" and cur is not None:
            w, h = cur["bbx"][:2]
            rows = [int(next(lines), 16) for _ in range(h)]
            glyphs[chr(cur["code"])] = (cur["dw"], *cur["bbx"], rows, ((w + 7) // 8) * 8)
            cur = None
    return glyphs, ascent


def name(im, text, y0, scale=2):
    """the name: Terminus at scale, each letter bobbing a little, a dark outline and a red shadow"""
    glyphs, ascent = bdf(FONT)
    width = sum(glyphs[c][0] for c in text) * scale
    mask = Image.new("L", im.size, 0)
    x = (240 - width) // 2
    for i, c in enumerate(text):
        dw, w, h, xo, yo, rows, bits = glyphs[c]
        bob = round(3 * math.sin(i * 1.3 + 0.5))
        top = y0 + (ascent - yo - h) * scale + bob
        for r, v in enumerate(rows):
            for col in range(w):
                if v >> (bits - 1 - col) & 1:
                    mask.paste(255, (x + (xo + col) * scale, top + r * scale,
                                     x + (xo + col + 1) * scale, top + (r + 1) * scale))
        x += dw * scale
    outline = mask.filter(ImageFilter.MaxFilter(5))
    shadow = Image.new("L", im.size, 0)
    shadow.paste(outline, (3, 3))
    im.paste(LIT, (0, 0), shadow)
    im.paste(DARK, (0, 0), outline)
    im.paste(FLESH, (0, 0), mask)


def main(dst, preview=None):
    im = art()
    name(im, "FMLarynx", 6)
    ImageDraw.Draw(im).rectangle([0, 224, 239, 239], fill=BG)   # (the version's line, written at boot)
    im.save(dst)
    if preview:
        im.resize((720, 720), Image.NEAREST).save(preview)
    print(f"splash_art: {dst}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
