#!/usr/bin/env python3
"""Manual art tool (NOT run by the build): bakes the reading-pet cat poses to
2-bit e-ink sprites in src/activities/stats/CatSprites.h. Requires Pillow.

Each pixel -> 0 transparent / 1 black ink / 2 gray (dithered on device).
Run: python3 scripts/bake_cat_sprites.py"""
import math
import os
from PIL import Image, ImageDraw

BLACK, GRAY, WHITE = 0, 175, 255
OUT = os.path.join(os.path.dirname(__file__), "..", "src", "activities", "stats", "CatSprites.h")


def new(W, H):
    im = Image.new("L", (W, H), WHITE)
    return im, ImageDraw.Draw(im)


def disc(d, cx, cy, r, fill, ow=0):
    d.ellipse([cx-r, cy-r, cx+r, cy+r], fill=fill)
    if ow:
        d.ellipse([cx-r, cy-r, cx+r, cy+r], outline=BLACK, width=ow)


def _catmull(p0, p1, p2, p3, t):
    t2 = t*t; t3 = t2*t
    def c(a, b, cc, dd):
        return 0.5*((2*b) + (-a+cc)*t + (2*a-5*b+4*cc-dd)*t2 + (-a+3*b-3*cc+dd)*t3)
    return (c(p0[0], p1[0], p2[0], p3[0]), c(p0[1], p1[1], p2[1], p3[1]))


def tail(d, ctrl, S, OW):
    baseW, tipW = int(.16*S), max(3, int(.06*S))
    P = [ctrl[0]] + list(ctrl) + [ctrl[-1]]
    cl = []
    for i in range(len(ctrl)-1):
        for s in range(14):
            cl.append(_catmull(P[i], P[i+1], P[i+2], P[i+3], s/14.0))
    cl.append(ctrl[-1])
    n = len(cl)
    left, right = [], []
    for i, (x, y) in enumerate(cl):
        if i == 0:
            dx, dy = cl[1][0]-x, cl[1][1]-y
        elif i == n-1:
            dx, dy = x-cl[i-1][0], y-cl[i-1][1]
        else:
            dx, dy = cl[i+1][0]-cl[i-1][0], cl[i+1][1]-cl[i-1][1]
        L = math.hypot(dx, dy) or 1.0
        nx, ny = -dy/L, dx/L
        w = (baseW + (tipW-baseW)*(i/(n-1.0)))/2.0
        left.append((x+nx*w, y+ny*w))
        right.append((x-nx*w, y-ny*w))
    poly = left + right[::-1]
    d.polygon(poly, fill=GRAY)
    d.line(poly + [poly[0]], fill=BLACK, width=OW, joint="curve")
    disc(d, int(round(cl[-1][0])), int(round(cl[-1][1])), max(2, tipW//2+1), GRAY, OW)


def head(d, hcx, hcy, hw, hh, OW):
    hx, hy = hcx-hw//2, hcy-hh//2
    earW, earH = int(.30*hw), int(.36*hh)
    for sgn in (-1, 1):
        ex = hcx+sgn*int(.30*hw)
        d.polygon([(ex-earW//2, hy+int(.20*hh)), (ex+earW//2, hy+int(.20*hh)), (ex+sgn*int(.05*hw), hy-earH)],
                  fill=GRAY, outline=BLACK)
    d.ellipse([hx, hy, hx+hw, hy+hh], fill=GRAY)
    d.ellipse([hx, hy, hx+hw, hy+hh], outline=BLACK, width=OW)
    for sgn in (-1, 1):
        ex = hcx+sgn*int(.30*hw)
        d.polygon([(ex-earW//4, hy+int(.12*hh)), (ex+earW//4, hy+int(.12*hh)), (ex+sgn*int(.05*hw), hy-int(earH*.5))],
                  fill=BLACK)
    my = hy+int(.14*hh)  # tabby "M"
    for k in (-2, -1, 0, 1, 2):
        x = hcx+k*int(.07*hw)
        h2 = int(.16*hh) if k % 2 == 0 else int(.10*hh)
        d.line([x, my, x, my+h2], fill=BLACK, width=OW)


def face(d, hcx, hcy, hw, hh, OW, sleeping=False):
    ey = hcy+int(.04*hh)
    if sleeping:
        for sgn in (-1, 1):
            ecx = hcx+sgn*int(.22*hw)
            d.arc([ecx-int(.10*hw), ey-int(.02*hh), ecx+int(.10*hw), ey+int(.12*hh)], 200, 340, fill=BLACK, width=OW+1)
    else:
        ew, eh = int(.20*hw), int(.26*hh)
        for sgn in (-1, 1):
            ecx = hcx+sgn*int(.22*hw)
            d.ellipse([ecx-ew//2, ey-eh//2, ecx+ew//2, ey+eh//2], fill=BLACK)
            cl = max(2, int(.045*hw))
            d.ellipse([ecx-cl, ey-eh//4-cl, ecx+cl, ey-eh//4+cl], fill=WHITE)
    ny = hcy+int(.22*hh)
    d.polygon([(hcx-int(.05*hw), ny), (hcx+int(.05*hw), ny), (hcx, ny+int(.06*hh))], fill=BLACK)
    d.line([hcx, ny+int(.06*hh), hcx, ny+int(.11*hh)], fill=BLACK, width=OW)
    d.arc([hcx-int(.11*hw), ny+int(.05*hh), hcx, ny+int(.15*hh)], 30, 150, fill=BLACK, width=OW)
    d.arc([hcx, ny+int(.05*hh), hcx+int(.11*hw), ny+int(.15*hh)], 30, 150, fill=BLACK, width=OW)
    for k in (-1, 0, 1):
        dy = k*int(.05*hh)
        d.line([hcx-int(.12*hw), ny+int(.02*hh), hcx-int(.48*hw), ny-int(.04*hh)+dy], fill=BLACK, width=max(1, OW-1))
        d.line([hcx+int(.12*hw), ny+int(.02*hh), hcx+int(.48*hw), ny-int(.04*hh)+dy], fill=BLACK, width=max(1, OW-1))


def cat_sitting(d, cx, by, S):
    OW = max(2, round(S*0.016))
    tail(d, [(cx+int(.20*S), by-int(.12*S)), (cx+int(.44*S), by-int(.16*S)), (cx+int(.58*S), by-int(.34*S)),
             (cx+int(.52*S), by-int(.54*S)), (cx+int(.36*S), by-int(.60*S))], S, OW)
    bw, bh = int(.60*S), int(.70*S)
    bx, byy = cx-bw//2, by-bh
    d.ellipse([bx, byy, bx+bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx+bw, by], outline=BLACK, width=OW)
    d.ellipse([cx-int(.16*S), byy+int(.40*bh), cx+int(.16*S), by-int(.04*bh)], fill=WHITE)
    pw = int(.22*S)
    for sgn in (-1, 1):
        pcx = cx+sgn*int(.15*S)
        d.ellipse([pcx-pw//2, by-int(.17*S), pcx+pw//2, by+int(.01*S)], fill=WHITE, outline=BLACK, width=OW)
        for t in (-1, 1):
            d.line([pcx+t*int(.035*S), by-int(.13*S), pcx+t*int(.035*S), by-int(.02*S)], fill=BLACK, width=max(1, OW-1))
    head(d, cx, byy-int(.10*S), int(.80*S), int(.74*S), OW)
    face(d, cx, byy-int(.10*S), int(.80*S), int(.74*S), OW)


def cat_loaf(d, cx, by, S):
    OW = max(2, round(S*0.016))
    bw, bh = int(1.06*S), int(.50*S)
    bx, byy = cx-bw//2, by-bh
    tail(d, [(cx+int(.40*S), by-int(.12*S)), (cx+int(.62*S), by-int(.10*S)), (cx+int(.76*S), by-int(.26*S)),
             (cx+int(.70*S), by-int(.44*S)), (cx+int(.56*S), by-int(.50*S))], S, OW)
    d.ellipse([bx, byy, bx+bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx+bw, by], outline=BLACK, width=OW)
    for i in range(2):
        pcx = cx-int(.16*S)+i*int(.20*S)
        d.ellipse([pcx-int(.10*S), by-int(.11*S), pcx+int(.10*S), by+int(.02*S)], fill=WHITE, outline=BLACK, width=OW)
    head(d, bx+int(.33*bw), byy-int(.04*S), int(.64*S), int(.60*S), OW)
    face(d, bx+int(.33*bw), byy-int(.04*S), int(.64*S), int(.60*S), OW)


def cat_kitten(d, cx, by, S):
    OW = max(2, round(S*0.016))
    bw, bh = int(.92*S), int(.58*S)
    bx, byy = cx-bw//2, by-bh
    tail(d, [(cx+int(.28*S), by-int(.10*S)), (cx+int(.02*S), by-int(.04*S)), (cx-int(.30*S), by-int(.06*S)),
             (cx-int(.48*S), by-int(.20*S)), (cx-int(.48*S), by-int(.38*S))], S, OW)
    d.ellipse([bx, byy, bx+bw, by], fill=GRAY)
    d.ellipse([bx, byy, bx+bw, by], outline=BLACK, width=OW)
    d.ellipse([cx-int(.12*S), byy+int(.42*bh), cx+int(.20*S), by-int(.04*bh)], fill=WHITE)
    head(d, cx-int(.16*S), by-int(.30*S), int(.58*S), int(.56*S), OW)
    face(d, cx-int(.16*S), by-int(.30*S), int(.58*S), int(.56*S), OW, sleeping=True)


def bake(func, S):
    im, d = new(440, 440)
    func(d, 220, 350, S)
    mask = im.point(lambda v: 255 if v < 250 else 0)
    bbox = mask.getbbox()
    im = im.crop(bbox)
    w, h = im.size
    px = im.load()
    grid = []
    for y in range(h):
        for x in range(w):
            v = px[x, y]
            grid.append(1 if v < 90 else (2 if v <= 235 else 0))
    nbytes = (w*h + 3) // 4
    data = bytearray(nbytes)
    for i, val in enumerate(grid):
        data[i >> 2] |= (val & 3) << ((i & 3) * 2)
    return w, h, bytes(data), grid


_BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def preview(sprites, fname):
    pad = 12
    tot_w = sum(s[0] for s in sprites.values()) + pad*(len(sprites)+1)
    tot_h = max(s[1] for s in sprites.values()) + 2*pad
    out = Image.new("L", (tot_w, tot_h), 220)
    x = pad
    for name, (w, h, data, grid) in sprites.items():
        cell = Image.new("L", (w, h), WHITE)
        cp = cell.load()
        for i, val in enumerate(grid):
            px, py = i % w, i // w
            if val == 1:
                cp[px, py] = BLACK
            elif val == 2:
                cp[px, py] = BLACK if _BAYER[py % 4][px % 4] < 5 else WHITE
        out.paste(cell, (x, tot_h-pad-h))
        x += w + pad
    out.resize((tot_w*3, tot_h*3), Image.NEAREST).save(fname)


def carr(name, data):
    return "static const uint8_t %s[] = {%s};\n" % (name, ", ".join(str(b) for b in data))


def main():
    poses = {"Kitten": (cat_kitten, 104), "Sitting": (cat_sitting, 92), "Loaf": (cat_loaf, 90)}
    sprites = {name: bake(fn, S) for name, (fn, S) in poses.items()}
    preview(sprites, "/tmp/cats_baked.png")
    lines = ["#pragma once\n\n#include <cstdint>\n\n",
             "// Generated by scripts/bake_cat_sprites.py -- 2 bits/pixel, row-major\n",
             "// (0 transparent, 1 black ink, 2 gray). Do not hand-edit.\n\n",
             "struct CatSprite {\n  const uint8_t* data;\n  uint16_t w;\n  uint16_t h;\n};\n\n"]
    for name, (w, h, data, _g) in sprites.items():
        lines.append(carr("kCat%sData" % name, data))
    lines.append("\n")
    for name, (w, h, _d, _g) in sprites.items():
        lines.append("static const CatSprite kCat%s = {kCat%sData, %d, %d};\n" % (name, name, w, h))
    with open(os.path.abspath(OUT), "w") as f:
        f.write("".join(lines))
    print("wrote", os.path.abspath(OUT))
    for name, (w, h, data, _g) in sprites.items():
        print("  %-8s %dx%d  %d bytes" % (name, w, h, len(data)))


main()
