#!/usr/bin/env python3
"""Manual art tool (NOT run by the build): procedurally draws the 11 reading-pet
evolution sprites and bakes them to 2-bit e-ink sprites in
src/activities/stats/CatSprites.h. Requires Pillow.

Same technique as the original cat baker: vector shapes on a grayscale canvas,
then quantized to 0 transparent / 1 black ink / 2 gray (dithered on device).
Run: python3 scripts/bake_pet_sprites.py"""
import math
import os

from PIL import Image, ImageDraw

BLACK, GRAY, WHITE = 0, 175, 255
OUT = os.path.join(os.path.dirname(__file__), "..", "src", "activities", "stats", "CatSprites.h")
MAX_W, MAX_H = 120, 138  # renderPet stacks name + 3 bars + status below the sprite


def new(W, H):
    im = Image.new("L", (W, H), WHITE)
    return im, ImageDraw.Draw(im)


def disc(d, cx, cy, r, fill, ow=0):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill)
    if ow:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=BLACK, width=ow)


def _catmull(p0, p1, p2, p3, t):
    t2 = t * t
    t3 = t2 * t

    def c(a, b, cc, dd):
        return 0.5 * ((2 * b) + (-a + cc) * t + (2 * a - 5 * b + 4 * cc - dd) * t2 + (-a + 3 * b - 3 * cc + dd) * t3)

    return (c(p0[0], p1[0], p2[0], p3[0]), c(p0[1], p1[1], p2[1], p3[1]))


def _spline(ctrl, steps=14):
    P = [ctrl[0]] + list(ctrl) + [ctrl[-1]]
    cl = []
    for i in range(len(ctrl) - 1):
        for s in range(steps):
            cl.append(_catmull(P[i], P[i + 1], P[i + 2], P[i + 3], s / float(steps)))
    cl.append(ctrl[-1])
    return cl


def tapered(d, ctrl, baseW, tipW, fill, OW, cap=True):
    cl = _spline(ctrl)
    n = len(cl)
    left, right = [], []
    for i, (x, y) in enumerate(cl):
        if i == 0:
            dx, dy = cl[1][0] - x, cl[1][1] - y
        elif i == n - 1:
            dx, dy = x - cl[i - 1][0], y - cl[i - 1][1]
        else:
            dx, dy = cl[i + 1][0] - cl[i - 1][0], cl[i + 1][1] - cl[i - 1][1]
        L = math.hypot(dx, dy) or 1.0
        nx, ny = -dy / L, dx / L
        w = (baseW + (tipW - baseW) * (i / (n - 1.0))) / 2.0
        left.append((x + nx * w, y + ny * w))
        right.append((x - nx * w, y - ny * w))
    poly = left + right[::-1]
    d.polygon(poly, fill=fill)
    d.line(poly + [poly[0]], fill=BLACK, width=OW, joint="curve")
    if cap:
        disc(d, int(round(cl[-1][0])), int(round(cl[-1][1])), max(2, tipW // 2 + 1), fill, OW)
    return cl


def tail(d, ctrl, S, OW, fill=GRAY):
    tapered(d, ctrl, int(.16 * S), max(3, int(.06 * S)), fill, OW)


# ---------------------------------------------------------------- cats
def head(d, hcx, hcy, hw, hh, OW):
    hx, hy = hcx - hw // 2, hcy - hh // 2
    earW, earH = int(.30 * hw), int(.36 * hh)
    for sgn in (-1, 1):
        ex = hcx + sgn * int(.30 * hw)
        d.polygon([(ex - earW // 2, hy + int(.20 * hh)), (ex + earW // 2, hy + int(.20 * hh)),
                   (ex + sgn * int(.05 * hw), hy - earH)], fill=GRAY, outline=BLACK)
    d.ellipse([hx, hy, hx + hw, hy + hh], fill=GRAY)
    d.ellipse([hx, hy, hx + hw, hy + hh], outline=BLACK, width=OW)
    for sgn in (-1, 1):
        ex = hcx + sgn * int(.30 * hw)
        d.polygon([(ex - earW // 4, hy + int(.12 * hh)), (ex + earW // 4, hy + int(.12 * hh)),
                   (ex + sgn * int(.05 * hw), hy - int(earH * .5))], fill=BLACK)
    my = hy + int(.14 * hh)
    for k in (-2, -1, 0, 1, 2):
        x = hcx + k * int(.07 * hw)
        h2 = int(.16 * hh) if k % 2 == 0 else int(.10 * hh)
        d.line([x, my, x, my + h2], fill=BLACK, width=OW)


def face(d, hcx, hcy, hw, hh, OW, sleeping=False):
    ey = hcy + int(.04 * hh)
    if sleeping:
        for sgn in (-1, 1):
            ecx = hcx + sgn * int(.22 * hw)
            d.arc([ecx - int(.10 * hw), ey - int(.02 * hh), ecx + int(.10 * hw), ey + int(.12 * hh)], 200, 340,
                  fill=BLACK, width=OW + 1)
    else:
        ew, eh = int(.20 * hw), int(.26 * hh)
        for sgn in (-1, 1):
            ecx = hcx + sgn * int(.22 * hw)
            d.ellipse([ecx - ew // 2, ey - eh // 2, ecx + ew // 2, ey + eh // 2], fill=BLACK)
            cl = max(2, int(.045 * hw))
            d.ellipse([ecx - cl, ey - eh // 4 - cl, ecx + cl, ey - eh // 4 + cl], fill=WHITE)
    ny = hcy + int(.22 * hh)
    d.polygon([(hcx - int(.05 * hw), ny), (hcx + int(.05 * hw), ny), (hcx, ny + int(.06 * hh))], fill=BLACK)
    d.line([hcx, ny + int(.06 * hh), hcx, ny + int(.11 * hh)], fill=BLACK, width=OW)
    d.arc([hcx - int(.11 * hw), ny + int(.05 * hh), hcx, ny + int(.15 * hh)], 30, 150, fill=BLACK, width=OW)
    d.arc([hcx, ny + int(.05 * hh), hcx + int(.11 * hw), ny + int(.15 * hh)], 30, 150, fill=BLACK, width=OW)
    for k in (-1, 0, 1):
        dy = k * int(.05 * hh)
        d.line([hcx - int(.12 * hw), ny + int(.02 * hh), hcx - int(.48 * hw), ny - int(.04 * hh) + dy], fill=BLACK,
               width=max(1, OW - 1))
        d.line([hcx + int(.12 * hw), ny + int(.02 * hh), hcx + int(.48 * hw), ny - int(.04 * hh) + dy], fill=BLACK,
               width=max(1, OW - 1))


def cat_sitting(d, cx, by, S):
    OW = max(2, round(S * 0.016))
    tail(d, [(cx + int(.20 * S), by - int(.12 * S)), (cx + int(.44 * S), by - int(.16 * S)),
             (cx + int(.58 * S), by - int(.34 * S)), (cx + int(.52 * S), by - int(.54 * S)),
             (cx + int(.36 * S), by - int(.60 * S))], S, OW)
    bw, bh = int(.60 * S), int(.70 * S)
    bx, byy = cx - bw // 2, by - bh
    d.ellipse([bx, byy, bx + bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx + bw, by], outline=BLACK, width=OW)
    d.ellipse([cx - int(.16 * S), byy + int(.40 * bh), cx + int(.16 * S), by - int(.04 * bh)], fill=WHITE)
    pw = int(.22 * S)
    for sgn in (-1, 1):
        pcx = cx + sgn * int(.15 * S)
        d.ellipse([pcx - pw // 2, by - int(.17 * S), pcx + pw // 2, by + int(.01 * S)], fill=WHITE, outline=BLACK,
                  width=OW)
        for t in (-1, 1):
            d.line([pcx + t * int(.035 * S), by - int(.13 * S), pcx + t * int(.035 * S), by - int(.02 * S)],
                   fill=BLACK, width=max(1, OW - 1))
    head(d, cx, byy - int(.10 * S), int(.80 * S), int(.74 * S), OW)
    face(d, cx, byy - int(.10 * S), int(.80 * S), int(.74 * S), OW)


def cat_loaf(d, cx, by, S):
    OW = max(2, round(S * 0.016))
    bw, bh = int(1.06 * S), int(.50 * S)
    bx, byy = cx - bw // 2, by - bh
    tail(d, [(cx + int(.40 * S), by - int(.12 * S)), (cx + int(.62 * S), by - int(.10 * S)),
             (cx + int(.76 * S), by - int(.26 * S)), (cx + int(.70 * S), by - int(.44 * S)),
             (cx + int(.56 * S), by - int(.50 * S))], S, OW)
    d.ellipse([bx, byy, bx + bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx + bw, by], outline=BLACK, width=OW)
    for i in range(2):
        pcx = cx - int(.16 * S) + i * int(.20 * S)
        d.ellipse([pcx - int(.10 * S), by - int(.11 * S), pcx + int(.10 * S), by + int(.02 * S)], fill=WHITE,
                  outline=BLACK, width=OW)
    head(d, bx + int(.33 * bw), byy - int(.04 * S), int(.64 * S), int(.60 * S), OW)
    face(d, bx + int(.33 * bw), byy - int(.04 * S), int(.64 * S), int(.60 * S), OW)


def cat_kitten(d, cx, by, S):
    OW = max(2, round(S * 0.016))
    bw, bh = int(.92 * S), int(.58 * S)
    bx, byy = cx - bw // 2, by - bh
    tail(d, [(cx + int(.30 * S), by - int(.10 * S)), (cx + int(.52 * S), by - int(.08 * S)),
             (cx + int(.64 * S), by - int(.24 * S)), (cx + int(.58 * S), by - int(.42 * S)),
             (cx + int(.44 * S), by - int(.50 * S))], S, OW)
    d.ellipse([bx, byy, bx + bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx + bw, by], outline=BLACK, width=OW)
    d.ellipse([cx - int(.12 * S), byy + int(.42 * bh), cx + int(.20 * S), by - int(.04 * bh)], fill=WHITE)
    head(d, cx - int(.16 * S), by - int(.30 * S), int(.58 * S), int(.56 * S), OW)
    face(d, cx - int(.16 * S), by - int(.30 * S), int(.58 * S), int(.56 * S), OW, sleeping=True)


# ---------------------------------------------------------------- tigers
def _tiger_head(d, hcx, hcy, hw, hh, OW, fierce):
    hx, hy = hcx - hw // 2, hcy - hh // 2
    for sgn in (-1, 1):  # rounded ears
        ex = hcx + sgn * int(.34 * hw)
        disc(d, ex, hy + int(.14 * hh), int(.14 * hw), GRAY, OW)
        disc(d, ex, hy + int(.16 * hh), int(.07 * hw), BLACK, 0)
    d.ellipse([hx, hy, hx + hw, hy + hh], fill=GRAY)
    d.ellipse([hx, hy, hx + hw, hy + hh], outline=BLACK, width=OW)
    d.ellipse([hcx - int(.40 * hw), hcy + int(.06 * hh), hcx + int(.40 * hw), hcy + int(.48 * hh)], fill=WHITE,
              outline=BLACK, width=max(1, OW - 1))  # muzzle
    for k in (-3, -2, -1, 1, 2, 3):  # forehead stripes
        x = hcx + k * int(.07 * hw)
        d.line([x, hy + int(.06 * hh), x, hy + int(.28 * hh)], fill=BLACK, width=OW)
    ey = hcy + int(.0 * hh)
    for sgn in (-1, 1):
        ecx = hcx + sgn * int(.20 * hw)
        d.ellipse([ecx - int(.08 * hw), ey - int(.07 * hh), ecx + int(.08 * hw), ey + int(.07 * hh)], fill=BLACK)
    ny = hcy + int(.20 * hh)
    d.polygon([(hcx - int(.07 * hw), ny), (hcx + int(.07 * hw), ny), (hcx, ny + int(.07 * hh))], fill=BLACK)
    d.line([hcx, ny + int(.07 * hh), hcx, ny + int(.13 * hh)], fill=BLACK, width=OW)
    d.arc([hcx - int(.14 * hw), ny + int(.04 * hh), hcx, ny + int(.17 * hh)], 25, 155, fill=BLACK, width=OW)
    d.arc([hcx, ny + int(.04 * hh), hcx + int(.14 * hw), ny + int(.17 * hh)], 25, 155, fill=BLACK, width=OW)
    if fierce:
        for sgn in (-1, 1):
            fx = hcx + sgn * int(.05 * hw)
            d.polygon([(fx, ny + int(.13 * hh)), (fx + sgn * int(.05 * hw), ny + int(.13 * hh)),
                       (fx + sgn * int(.02 * hw), ny + int(.26 * hh))], fill=WHITE, outline=BLACK)


def tiger(d, cx, by, S, fierce=False, baby=False):
    OW = max(2, round(S * 0.016))
    tail(d, [(cx + int(.22 * S), by - int(.10 * S)), (cx + int(.46 * S), by - int(.14 * S)),
             (cx + int(.60 * S), by - int(.32 * S)), (cx + int(.54 * S), by - int(.52 * S)),
             (cx + int(.40 * S), by - int(.58 * S))], S, OW)
    bw, bh = int(.78 * S), int(.74 * S)
    bx, byy = cx - bw // 2, by - bh
    d.ellipse([bx, byy, bx + bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx + bw, by], outline=BLACK, width=OW)
    d.ellipse([cx - int(.17 * S), byy + int(.42 * bh), cx + int(.17 * S), by - int(.04 * bh)], fill=WHITE)
    for k in (-2, -1, 1, 2):  # body stripes
        sx = cx + k * int(.15 * S)
        d.line([(sx, byy + int(.16 * bh)), (sx + int(.02 * S), byy + int(.64 * bh))], fill=BLACK, width=OW)
    pw = int(.24 * S)
    for sgn in (-1, 1):
        pcx = cx + sgn * int(.16 * S)
        d.ellipse([pcx - pw // 2, by - int(.16 * S), pcx + pw // 2, by + int(.01 * S)], fill=WHITE, outline=BLACK,
                  width=OW)
    hw = int((1.02 if baby else .92) * S)
    hh = int((.92 if baby else .80) * S)
    _tiger_head(d, cx, byy - int(.04 * S), hw, hh, OW, fierce)


def tiger_loaf(d, cx, by, S):
    OW = max(2, round(S * 0.016))
    bw, bh = int(1.04 * S), int(.52 * S)
    bx, byy = cx - bw // 2, by - bh
    tail(d, [(cx + int(.40 * S), by - int(.12 * S)), (cx + int(.62 * S), by - int(.10 * S)),
             (cx + int(.76 * S), by - int(.26 * S)), (cx + int(.70 * S), by - int(.44 * S)),
             (cx + int(.56 * S), by - int(.50 * S))], S, OW)
    d.ellipse([bx, byy, bx + bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx + bw, by], outline=BLACK, width=OW)
    for i in range(2):  # front paws
        pcx = cx - int(.20 * S) + i * int(.22 * S)
        d.ellipse([pcx - int(.11 * S), by - int(.12 * S), pcx + int(.11 * S), by + int(.02 * S)], fill=WHITE,
                  outline=BLACK, width=OW)
    for k in (-1, 0, 1, 2):  # body stripes along the back
        sx = cx + int(.10 * S) + k * int(.13 * S)
        if bx + int(.34 * bw) < sx < bx + bw - int(.06 * S):
            d.line([(sx, byy + int(.16 * bh)), (sx + int(.03 * S), byy + int(.86 * bh))], fill=BLACK, width=OW)
    _tiger_head(d, bx + int(.30 * bw), byy - int(.02 * S), int(.70 * S), int(.64 * S), OW, False)


# ---------------------------------------------------------------- dragons
def _wing(d, sx, sy, dirx, S, OW, span):
    L = span * S
    shoulder = (sx, sy)
    top = (sx + dirx * int(.16 * L), sy - int(.86 * L))
    tip = (sx + dirx * int(.60 * L), sy - int(.56 * L))
    f1 = (sx + dirx * int(.44 * L), sy - int(.22 * L))
    f2 = (sx + dirx * int(.54 * L), sy + int(.10 * L))
    base = (sx + dirx * int(.12 * L), sy + int(.16 * L))
    poly = [shoulder, top, tip, (sx + dirx * int(.42 * L), sy - int(.34 * L)), f1,
            (sx + dirx * int(.44 * L), sy - int(.04 * L)), f2, base]
    d.polygon(poly, fill=GRAY)
    d.line(poly + [poly[0]], fill=BLACK, width=OW, joint="curve")
    for fp in (top, tip, f1, f2):
        d.line([shoulder, fp], fill=BLACK, width=max(1, OW - 1))


def _horns(d, hcx, topy, hw, OW, n=2):
    spread = [(-.34, .9), (.34, .9), (-.12, 1.15), (.12, 1.15)][:n]
    for fx, hscale in spread:
        bx = hcx + int(fx * hw)
        d.polygon([(bx - int(.07 * hw), topy), (bx + int(.07 * hw), topy),
                   (bx + int(fx * .5 * hw), topy - int(hscale * .42 * hw))], fill=GRAY, outline=BLACK)


def _ridge(d, ctrl, S, OW, k=5):
    cl = _spline(ctrl, steps=6)
    n = len(cl)
    step = max(1, n // (k + 1))
    h = int(.10 * S)
    for i in range(step, n - step, step):
        x, y = cl[i]
        px, py = cl[i - 1]
        dx, dy = x - px, y - py
        L = math.hypot(dx, dy) or 1.0
        nx, ny = -dy / L, dx / L
        d.polygon([(x - dx * 0.4, y - dy * 0.4), (x + dx * 0.4, y + dy * 0.4),
                   (x + nx * h, y + ny * h)], fill=GRAY, outline=BLACK)


def dragon_egg(d, cx, by, S):
    OW = max(2, round(S * 0.016))
    ew, eh = int(.80 * S), int(.98 * S)
    ex, ey = cx - ew // 2, by - eh
    d.ellipse([ex, ey, ex + ew, by], fill=GRAY)
    d.ellipse([ex, ey, ex + ew, by], outline=BLACK, width=OW)
    sw = int(.16 * S)  # overlapping scale shingles, kept inside the shell outline
    rows = 7
    for r in range(1, rows):
        ry = ey + int(eh * r / float(rows))
        ty = (ry - .09 * S - (ey + eh / 2.0)) / (eh / 2.0)
        halfw = (ew / 2.0) * math.sqrt(max(0.0, 1.0 - ty * ty)) - int(.11 * S)
        if halfw < sw:
            continue
        x = cx - halfw + (sw / 2.0 if r % 2 else 0.0)
        while x + sw <= cx + halfw:
            d.arc([int(x), int(ry - .10 * S), int(x) + sw, int(ry + .04 * S)], 180, 360, fill=BLACK,
                  width=max(1, OW - 1))
            x += sw
    topy = ey + int(.06 * eh)  # ridge bumps along the crown
    for k in (-1, 0, 1):
        rx = cx + k * int(.12 * S)
        d.polygon([(rx - int(.05 * S), topy), (rx + int(.05 * S), topy), (rx, topy - int(.10 * S))], fill=GRAY,
                  outline=BLACK)
    zig = [(-.16, .14), (-.05, .19), (-.10, .26), (.03, .21), (-.01, .30), (.10, .23)]
    pts = [(cx + int(zx * ew), ey + int(zy * eh)) for zx, zy in zig]
    d.line(pts, fill=BLACK, width=OW, joint="curve")
    d.ellipse([ex, ey, ex + ew, by], outline=BLACK, width=OW)  # clean shell edge over the scales


def dragon_hatchling(d, cx, by, S):
    OW = max(2, round(S * 0.016))
    # cracked egg base
    d.arc([cx - int(.42 * S), by - int(.30 * S), cx + int(.42 * S), by + int(.10 * S)], 0, 180, fill=BLACK, width=OW)
    d.polygon([(cx - int(.42 * S), by - int(.10 * S)), (cx - int(.30 * S), by - int(.18 * S)),
               (cx - int(.18 * S), by - int(.10 * S)), (cx - int(.04 * S), by - int(.18 * S)),
               (cx + int(.10 * S), by - int(.10 * S)), (cx + int(.24 * S), by - int(.18 * S)),
               (cx + int(.42 * S), by - int(.10 * S)), (cx + int(.42 * S), by + int(.06 * S)),
               (cx - int(.42 * S), by + int(.06 * S))], fill=WHITE, outline=BLACK)
    bcy = by - int(.34 * S)
    disc(d, cx, bcy, int(.30 * S), GRAY, OW)  # body
    _wing(d, cx - int(.06 * S), bcy - int(.08 * S), -1, S, OW, span=.42)
    _wing(d, cx + int(.06 * S), bcy - int(.08 * S), 1, S, OW, span=.42)
    hcy = bcy - int(.30 * S)
    disc(d, cx, hcy, int(.26 * S), GRAY, OW)  # big head
    _horns(d, cx, hcy - int(.18 * S), int(.5 * S), OW, n=2)
    for sgn in (-1, 1):  # big eyes
        disc(d, cx + sgn * int(.10 * S), hcy, max(2, int(.06 * S)), BLACK)
        disc(d, cx + sgn * int(.10 * S) - 1, hcy - 1, max(1, int(.02 * S)), WHITE)
    disc(d, cx, hcy + int(.12 * S), max(1, int(.025 * S)), BLACK)  # snout dot


def dragon_young(d, cx, by, S):
    OW = max(2, round(S * 0.016))
    tail(d, [(cx + int(.14 * S), by - int(.08 * S)), (cx + int(.40 * S), by - int(.06 * S)),
             (cx + int(.56 * S), by - int(.22 * S)), (cx + int(.50 * S), by - int(.42 * S))], S, OW)
    bw, bh = int(.56 * S), int(.50 * S)
    bx, byy = cx - int(.30 * S), by - bh
    _wing(d, cx + int(.02 * S), byy + int(.16 * bh), -1, S, OW, span=.66)
    d.ellipse([bx, byy, bx + bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx + bw, by], outline=BLACK, width=OW)
    for sgn in (-1, 1):  # legs
        d.ellipse([cx + sgn * int(.04 * S) - int(.07 * S), by - int(.10 * S), cx + sgn * int(.04 * S) + int(.07 * S),
                   by + int(.02 * S)], fill=GRAY, outline=BLACK, width=OW)
    hcx, hcy = cx - int(.30 * S), byy - int(.18 * S)
    d.line([(cx - int(.16 * S), byy + int(.10 * bh)), (hcx, hcy)], fill=GRAY, width=int(.20 * S), joint="curve")
    disc(d, hcx, hcy, int(.20 * S), GRAY, OW)  # head
    d.polygon([(hcx - int(.06 * S), hcy - int(.05 * S)), (hcx - int(.30 * S), hcy + int(.02 * S)),
               (hcx - int(.06 * S), hcy + int(.12 * S))], fill=GRAY, outline=BLACK)  # snout
    _horns(d, hcx + int(.04 * S), hcy - int(.14 * S), int(.4 * S), OW, n=2)
    disc(d, hcx - int(.02 * S), hcy - int(.02 * S), max(1, int(.035 * S)), BLACK)


def dragon_adult(d, cx, by, S, elder=False):
    OW = max(2, round(S * 0.016))
    tcl = [(cx + int(.10 * S), by - int(.10 * S)), (cx + int(.40 * S), by - int(.04 * S)),
           (cx + int(.60 * S), by - int(.20 * S)), (cx + int(.56 * S), by - int(.44 * S)),
           (cx + int(.40 * S), by - int(.52 * S))]
    tail(d, tcl, S, OW)
    _ridge(d, tcl, S, OW, k=4)
    bw, bh = int(.62 * S), int(.52 * S)
    bx, byy = cx - int(.34 * S), by - bh
    _wing(d, cx - int(.04 * S), byy + int(.06 * bh), -1, S, OW, span=(1.0 if elder else .86))
    _wing(d, cx + int(.10 * S), byy + int(.02 * bh), 1, S, OW, span=(.78 if elder else .66))
    d.ellipse([bx, byy, bx + bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx + bw, by], outline=BLACK, width=OW)
    for sgn in (-1, 1):
        lx = cx + sgn * int(.08 * S)
        d.ellipse([lx - int(.08 * S), by - int(.12 * S), lx + int(.08 * S), by + int(.02 * S)], fill=GRAY,
                  outline=BLACK, width=OW)
    hcx, hcy = cx - int(.40 * S), byy - int(.34 * S)
    ncl = [(cx - int(.18 * S), byy + int(.10 * bh)), (cx - int(.34 * S), byy - int(.10 * S)), (hcx, hcy)]
    tapered(d, ncl, int(.26 * S), int(.18 * S), GRAY, OW, cap=False)
    _ridge(d, ncl, S, OW, k=3)
    hw, hh = int(.34 * S), int(.30 * S)
    d.ellipse([hcx - hw // 2, hcy - hh // 2, hcx + hw // 2, hcy + hh // 2], fill=GRAY, outline=BLACK, width=OW)
    d.polygon([(hcx - int(.10 * hw), hcy - int(.16 * hh)), (hcx - int(1.0 * hw), hcy + int(.04 * hh)),
               (hcx - int(.10 * hw), hcy + int(.34 * hh))], fill=GRAY, outline=BLACK)  # snout
    disc(d, hcx - int(.7 * hw), hcy + int(.02 * hh), max(1, int(.03 * S)), BLACK)  # nostril
    _horns(d, hcx + int(.12 * hw), hcy - int(.34 * hh), hw, OW, n=(4 if elder else 2))
    disc(d, hcx - int(.02 * hw), hcy - int(.06 * hh), max(1, int(.04 * S)), BLACK)  # eye
    if elder:
        for sgn in (-1, 1):  # brow + beard
            d.line([(hcx - int(.2 * hw), hcy + int(.34 * hh)), (hcx - int(.2 * hw) + sgn * int(.05 * S),
                    hcy + int(.78 * hh))], fill=BLACK, width=OW)


def bake(func, S):
    im, d = new(540, 540)
    func(d, 270, 430, S)
    mask = im.point(lambda v: 255 if v < 250 else 0)
    bbox = mask.getbbox()
    im = im.crop(bbox)
    w, h = im.size
    sc = min(1.0, MAX_W / float(w), MAX_H / float(h))
    blackT = 90
    if sc < 1.0:
        im = im.resize((max(1, int(round(w * sc))), max(1, int(round(h * sc)))), Image.LANCZOS)
        w, h = im.size
        blackT = 135  # keep downscaled outlines dark after antialiasing
    px = im.load()
    grid = []
    for y in range(h):
        for x in range(w):
            v = px[x, y]
            grid.append(1 if v < blackT else (2 if v <= 238 else 0))
    data = bytearray((w * h + 3) // 4)
    for i, val in enumerate(grid):
        data[i >> 2] |= (val & 3) << ((i & 3) * 2)
    return w, h, bytes(data)


def carr(name, data):
    return "static const uint8_t %s[] = {%s};\n" % (name, ", ".join(str(b) for b in data))


SPRITES = [
    ("kCatKitten", "kCatKittenData", cat_kitten, 78),
    ("kCatTeen", "kCatTeenData", cat_loaf, 96),
    ("kCatAdult", "kCatAdultData", cat_sitting, 108),
    ("kTigerCub", "kTigerCubData", lambda d, cx, by, S: tiger(d, cx, by, S, baby=True), 92),
    ("kTigerTeen", "kTigerTeenData", tiger_loaf, 100),
    ("kTigerAdult", "kTigerAdultData", lambda d, cx, by, S: tiger(d, cx, by, S, fierce=True), 116),
    ("kDragonEgg", "kDragonEggData", dragon_egg, 116),
    ("kDragonHatch", "kDragonHatchData", dragon_hatchling, 92),
    ("kDragonJuv", "kDragonJuvData", dragon_young, 104),
    ("kDragonAdult", "kDragonAdultData", lambda d, cx, by, S: dragon_adult(d, cx, by, S), 110),
    ("kDragonElder", "kDragonElderData", lambda d, cx, by, S: dragon_adult(d, cx, by, S, elder=True), 118),
]


def main():
    baked = []
    for sym, arr, fn, S in SPRITES:
        w, h, data = bake(fn, S)
        flag = "  <-- OVER ENVELOPE" if (w > MAX_W or h > MAX_H) else ""
        baked.append((sym, arr, w, h, data))
        print("  %-14s %dx%d  %d bytes%s" % (sym, w, h, len(data), flag))
    lines = [
        "#pragma once\n\n#include <cstdint>\n\n",
        "// Generated by scripts/bake_pet_sprites.py -- 2 bits/pixel, row-major\n",
        "// (0 transparent, 1 black ink, 2 gray). Do not hand-edit.\n\n",
        "struct CatSprite {\n  const uint8_t* data;\n  uint16_t w;\n  uint16_t h;\n};\n\n",
    ]
    for sym, arr, w, h, data in baked:
        lines.append(carr(arr, data))
    lines.append("\n")
    for sym, arr, w, h, data in baked:
        lines.append("static const CatSprite %s = {%s, %d, %d};\n" % (sym, arr, w, h))
    with open(os.path.abspath(OUT), "w") as f:
        f.write("".join(lines))
    print("wrote", os.path.abspath(OUT))


main()
