#!/usr/bin/env python3
"""
Generate 1-bit E-ink mockup PNGs for docs/UX_REDESIGN.md.

Renders 480×800 portrait monochrome bitmaps at the device's native
resolution — the same single-buffer 1-bit framebuffer the firmware
draws into — so the result is a faithful approximation of what the
panel actually shows, not a marketing render.

Mockups produced:
  docs/images/mockups/home.png          — §2.2.1 home redesign
  docs/images/mockups/series-viewer.png — §2.2.2 series viewer redesign
  docs/images/mockups/stats.png         — §2.6.1 stats redesign

Run from repo root:
  python3 scripts/generate_mockups.py
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 480, 800
WHITE, BLACK = 255, 0

OUTPUT_DIR = Path(__file__).resolve().parent.parent / "docs" / "images" / "mockups"


# --------------------------------------------------------------------
# Font loading — best-effort on macOS, falls back to DejaVu (Linux/CI).
# --------------------------------------------------------------------

_FONT_CANDIDATES = {
    "regular": [
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ],
    "bold": [
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ],
    "italic": [
        "/System/Library/Fonts/Supplemental/Arial Italic.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
    ],
}


def font(size, weight="regular"):
    for path in _FONT_CANDIDATES.get(weight, _FONT_CANDIDATES["regular"]):
        try:
            return ImageFont.truetype(path, size)
        except (OSError, IOError):
            continue
    return ImageFont.load_default()


# --------------------------------------------------------------------
# Drawing primitives — built from rectangles/lines/polygons so glyphs
# render identically across systems regardless of installed fonts.
# --------------------------------------------------------------------

def new_canvas():
    """Grayscale canvas; we threshold to 1-bit at save time."""
    img = Image.new("L", (W, H), WHITE)
    return img, ImageDraw.Draw(img)


def save_1bit(img, path):
    bw = img.convert("1", dither=Image.NONE)
    bw.save(path, "PNG")
    print(f"  → {path.relative_to(OUTPUT_DIR.parent.parent)}  ({W}×{H} 1-bit)")


def hairline(draw, y, x0=0, x1=W):
    draw.line([(x0, y), (x1, y)], fill=BLACK, width=1)


def text(draw, xy, s, fnt, anchor="lt", fill=BLACK):
    draw.text(xy, s, font=fnt, fill=fill, anchor=anchor)


def text_w(draw, s, fnt):
    bbox = draw.textbbox((0, 0), s, font=fnt)
    return bbox[2] - bbox[0]


def battery_icon(draw, x, y, percent, w=26, h=12):
    draw.rectangle([(x, y), (x + w, y + h)], outline=BLACK, width=1)
    draw.rectangle([(x + w + 1, y + 3), (x + w + 3, y + h - 3)], fill=BLACK)
    fill_w = max(0, int((w - 4) * percent / 100))
    if fill_w > 0:
        draw.rectangle([(x + 2, y + 2), (x + 2 + fill_w, y + h - 2)], fill=BLACK)


def progress_bar(draw, x, y, w, h, percent):
    draw.rectangle([(x, y), (x + w, y + h)], outline=BLACK, width=1)
    fill_w = int((w - 2) * percent / 100)
    if fill_w > 0:
        draw.rectangle([(x + 1, y + 1), (x + fill_w, y + h - 1)], fill=BLACK)


def sparkline(draw, x, y, w, h, values):
    if not values:
        return
    mx = max(values) or 1
    n = len(values)
    spacing = w / n
    bar_w = max(1, int(spacing) - 1)
    for i, v in enumerate(values):
        bar_h = max(1, int(h * v / mx))
        bx = x + int(i * spacing)
        by = y + (h - bar_h)
        draw.rectangle([(bx, by), (bx + bar_w, y + h)], fill=BLACK)


def left_arrow(draw, x, y, size=8, fill=BLACK):
    draw.polygon([(x + size, y), (x + size, y + 2 * size), (x, y + size)], fill=fill)


def right_arrow(draw, x, y, size=8, fill=BLACK):
    draw.polygon([(x, y), (x, y + 2 * size), (x + size, y + size)], fill=fill)


def up_arrow(draw, x, y, size=8, fill=BLACK):
    draw.polygon([(x, y + size), (x + 2 * size, y + size), (x + size, y)], fill=fill)


def down_arrow(draw, x, y, size=8, fill=BLACK):
    draw.polygon([(x, y), (x + 2 * size, y), (x + size, y + size)], fill=fill)


def updown_arrows(draw, x, y, size=8, fill=BLACK, gap=3):
    up_arrow(draw, x, y, size, fill)
    down_arrow(draw, x, y + size + gap, size, fill)


def check_mark(draw, x, y, size=12, fill=BLACK, line_w=2):
    draw.line([(x + 1, y + size // 2),
               (x + size // 3, y + size - 2)], fill=fill, width=line_w)
    draw.line([(x + size // 3, y + size - 2),
               (x + size - 1, y + 1)], fill=fill, width=line_w)


# --------------------------------------------------------------------
# Cover placeholders — abstract book-cover shapes so we don't fabricate
# fictional artwork. Title block + suggested text lines below.
# --------------------------------------------------------------------

def cover(draw, x, y, w, h, title=None, lines=True):
    draw.rectangle([(x, y), (x + w, y + h)], outline=BLACK, width=2)
    band_h = h // 4
    draw.rectangle([(x + 3, y + 3), (x + w - 3, y + 3 + band_h)], fill=BLACK)
    if title:
        title_font = font(max(9, min(w // 7, 16)), "bold")
        text(draw, (x + w // 2, y + 3 + band_h // 2),
             title, title_font, anchor="mm", fill=WHITE)
    if lines:
        # 4-6 suggestion lines in the lower portion
        body_y0 = y + band_h + 12
        body_y1 = y + h - 6
        n = max(3, (body_y1 - body_y0) // 12)
        for i in range(n):
            ly = body_y0 + i * 12
            if ly + 1 < body_y1:
                # alternate line lengths to suggest text
                offset = 6 if i % 2 == 0 else 14
                draw.line([(x + 6, ly), (x + w - offset, ly)],
                          fill=BLACK, width=1)


def thumb_with_badge(draw, x, y, w, h, badge=None):
    """Recents-grid thumbnail with optional numeric stack badge."""
    cover(draw, x, y, w, h, title=None, lines=True)
    if badge is not None:
        bx, by = x + w - 22, y + 4
        draw.ellipse([(bx, by), (bx + 18, by + 18)],
                     fill=WHITE, outline=BLACK, width=2)
        text(draw, (bx + 9, by + 9), str(badge),
             font(11, "bold"), anchor="mm", fill=BLACK)


# --------------------------------------------------------------------
# Footer button-hint row.
# Hints are a list of (glyph_drawer, label).
# --------------------------------------------------------------------

def button_hint(draw, y, hints):
    fnt = font(12, "bold")
    label_widths = [(g, l, text_w(draw, l, fnt) + 22) for g, l in hints]
    total = sum(w for _, _, w in label_widths)
    spacing = max(20, (W - total) // (len(label_widths) + 1))
    x = spacing
    for glyph, label, w in label_widths:
        glyph(draw, x, y + 3)
        text(draw, (x + 22, y + 4), label, fnt)
        x += w + spacing


# ====================================================================
# Screen 1 — Home redesign (§2.2.1)
# ====================================================================

def render_home():
    img, draw = new_canvas()

    # --- Header ---
    text(draw, (16, 12), "AALU", font(15, "bold"))
    battery_icon(draw, W - 82, 16, 87)
    text(draw, (W - 18, 20), "87%", font(12, "bold"), anchor="rm")
    hairline(draw, 42)

    # --- Hero block (y = 60..330) ---
    # Cover left
    cx, cy, cw, ch = 24, 64, 150, 220
    cover(draw, cx, cy, cw, ch, title="IRON FLAME")

    # Right column
    rx = cx + cw + 22

    text(draw, (rx, 64), "IRON FLAME", font(24, "bold"))
    text(draw, (rx, 102), "Rebecca Yarros", font(15))
    text(draw, (rx, 128), "The Empyrean · Book 2", font(13, "italic"))

    # Progress bar
    pb_x, pb_y, pb_w, pb_h = rx, 170, 254, 12
    progress_bar(draw, pb_x, pb_y, pb_w, pb_h, 39)
    text(draw, (rx, 192), "39%", font(22, "bold"))

    # Time-left line (the new signal we propose adding)
    text(draw, (rx, 234), "~6h to finish", font(12, "bold"))
    text(draw, (rx, 252), "1h 12m today", font(12))

    # --- Section divider + label ---
    hairline(draw, 312)
    text(draw, (W // 2, 322), "RECENTS",
         font(10, "bold"), anchor="mt")

    # --- 4×2 thumbnail grid ---
    tw, th = 96, 132
    grid_top = 358
    gap = (W - 4 * tw) // 5
    badges = [
        [3, 3, None, 8],
        [16, None, None, 9],
    ]
    for row_i, row in enumerate(badges):
        y = grid_top + row_i * (th + 14)
        for col_i, b in enumerate(row):
            x = gap + col_i * (tw + gap)
            thumb_with_badge(draw, x, y, tw, th, badge=b)

    # --- Bottom menu (4 equal tiles) ---
    menu_top = H - 70
    menu_h = 48
    labels = ["Files", "Library", "Transfer", "Settings"]
    tile_gap = 10
    tile_w = (W - 5 * tile_gap) // 4
    for i, label in enumerate(labels):
        x = tile_gap + i * (tile_w + tile_gap)
        draw.rectangle([(x, menu_top), (x + tile_w, menu_top + menu_h)],
                       outline=BLACK, width=2)
        text(draw, (x + tile_w // 2, menu_top + menu_h // 2),
             label, font(13, "bold"), anchor="mm")

    save_1bit(img, OUTPUT_DIR / "home.png")


# ====================================================================
# Screen 2 — Series Viewer redesign (§2.2.2)
# ====================================================================

def render_series_viewer():
    img, draw = new_canvas()

    # --- Header ---
    left_arrow(draw, 16, 16, size=7)
    text(draw, (34, 12), "Series", font(15, "bold"))
    battery_icon(draw, W - 82, 16, 86)
    text(draw, (W - 18, 20), "86%", font(12, "bold"), anchor="rm")
    hairline(draw, 42)

    # --- Title block ---
    text(draw, (W // 2, 60), "Dungeon Crawler Carl",
         font(20, "bold"), anchor="mt")
    text(draw, (W // 2, 92), "Matt Dinniman · 8 books · 2 finished",
         font(12), anchor="mt")
    hairline(draw, 122)

    # --- 3-col grid of covers, status glyphs, 2-line titles ---
    cols = 3
    cw, ch = 116, 158
    gap = (W - cols * cw) // (cols + 1)
    label_h = 36
    grid_top = 140
    row_gap = 18

    books = [
        ("Dungeon Crawler", "Carl", "check"),
        ("Carl's Doomsday", "Scenario", "check"),
        ("The Dungeon", "Anarchist's…", "reading"),
        ("The Gate", "of the Feral…", "4"),
        ("The Butcher's", "Masquerade", "5"),
        ("The Eye of", "the Bedlam…", "6"),
        ("This Inevitable", "Ruin", "7"),
        ("A Parade of", "Horribles", "8"),
    ]

    for i, (t1, t2, status) in enumerate(books):
        row = i // cols
        col = i % cols
        x = gap + col * (cw + gap)
        y = grid_top + row * (ch + label_h + row_gap)

        cover(draw, x, y, cw, ch)

        # Status badge in top-right corner
        bx, by = x + cw - 24, y + 4
        if status == "check":
            draw.ellipse([(bx, by), (bx + 22, by + 22)], fill=BLACK)
            check_mark(draw, bx + 5, by + 5, size=12, fill=WHITE, line_w=2)
        elif status == "reading":
            draw.ellipse([(bx, by), (bx + 22, by + 22)], fill=BLACK)
            right_arrow(draw, bx + 7, by + 6, size=5, fill=WHITE)
        else:
            draw.ellipse([(bx, by), (bx + 22, by + 22)],
                         fill=WHITE, outline=BLACK, width=2)
            text(draw, (bx + 11, by + 11), status,
                 font(11, "bold"), anchor="mm", fill=BLACK)

        # Title under cover, 2 lines
        text(draw, (x + cw // 2, y + ch + 4), t1,
             font(11, "bold"), anchor="mt")
        text(draw, (x + cw // 2, y + ch + 18), t2,
             font(11), anchor="mt")

    # --- Footer hints ---
    hairline(draw, H - 50)
    button_hint(draw, H - 32, [
        (left_arrow, "Back"),
        (right_arrow, "Open"),
        (updown_arrows, "Pick"),
    ])

    save_1bit(img, OUTPUT_DIR / "series-viewer.png")


# ====================================================================
# Screen 3 — Stats redesign (§2.6.1)
# ====================================================================

def render_stats():
    img, draw = new_canvas()

    # --- Header ---
    left_arrow(draw, 16, 16, size=7)
    text(draw, (34, 12), "Library Stats", font(15, "bold"))
    battery_icon(draw, W - 82, 16, 86)
    text(draw, (W - 18, 20), "86%", font(12, "bold"), anchor="rm")
    hairline(draw, 42)

    # --- Tab control (explicit) ---
    tab_y = 60
    tab_w, tab_h = 86, 30
    # Reading (selected, inverted)
    rx = W // 2 - tab_w - 4
    draw.rectangle([(rx, tab_y), (rx + tab_w, tab_y + tab_h)],
                   fill=BLACK)
    text(draw, (rx + tab_w // 2, tab_y + tab_h // 2),
         "Reading", font(13, "bold"), anchor="mm", fill=WHITE)
    # Finished (unselected)
    fx = W // 2 + 4
    draw.rectangle([(fx, tab_y), (fx + tab_w, tab_y + tab_h)],
                   outline=BLACK, width=2)
    text(draw, (fx + tab_w // 2, tab_y + tab_h // 2),
         "Finished", font(13), anchor="mm")

    hairline(draw, 110)

    # --- Year/month summary block ---
    text(draw, (24, 124), "THIS YEAR", font(10, "bold"))
    text(draw, (24, 146), "47h", font(28, "bold"))
    text(draw, (108, 158), "·  12 books finished", font(14), anchor="lb")

    text(draw, (24, 200), "THIS MONTH", font(10, "bold"))
    text(draw, (24, 220), "4h 22m", font(20, "bold"))
    text(draw, (130, 230), "·  pace", font(12), anchor="lb")
    # Sparkline of last 30 days reading minutes
    spark_vals = [3, 5, 4, 6, 7, 8, 9, 8, 6, 4, 3, 5, 7, 9, 11, 8,
                  6, 4, 5, 7, 8, 6, 4, 3, 4, 6, 8, 9, 7, 5]
    sparkline(draw, 184, 212, 130, 22, spark_vals)
    text(draw, (322, 230), "(last 30d)", font(10), anchor="lb")

    hairline(draw, 256)

    # --- Currently-reading book list ---
    list_top = 270
    row_h = 110
    books = [
        ("IRON", "Iron Flame", "Rebecca Yarros",
         "39% · 6h 24m · ~6h left"),
        ("PHM", "Project Hail Mary", "Andy Weir",
         "72% · 9h 12m · ~3h left"),
        ("3BP", "The Three-Body Problem", "Liu Cixin",
         "18% · 2h 06m · ~9h left"),
        ("CATCH", "Catch-22", "Joseph Heller",
         "55% · 4h 41m · ~4h left"),
    ]
    for i, (cover_t, title, author, prog) in enumerate(books):
        y = list_top + i * row_h
        cover(draw, 24, y, 74, 92, title=cover_t)
        text(draw, (114, y + 6), title, font(15, "bold"))
        text(draw, (114, y + 30), author, font(12))
        text(draw, (114, y + 56), prog, font(12, "bold"))
        # progress bar
        progress_bar(draw, 114, y + 76, 280, 8,
                     int(prog.split("%")[0]))

    # --- Footer hints ---
    hairline(draw, H - 50)
    # Left/right combined glyph for "switch tab"
    def lr_arrows(d, x, y, size=8, fill=BLACK):
        left_arrow(d, x, y, size, fill)
        right_arrow(d, x + size + 6, y, size, fill)

    button_hint(draw, H - 32, [
        (left_arrow, "Back"),
        (lr_arrows, "Switch tab"),
        (updown_arrows, "Scroll"),
    ])

    save_1bit(img, OUTPUT_DIR / "stats.png")


# --------------------------------------------------------------------

def main():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Generating mockups → {OUTPUT_DIR.relative_to(OUTPUT_DIR.parent.parent)}/")
    render_home()
    render_series_viewer()
    render_stats()
    print("Done.")


if __name__ == "__main__":
    main()
