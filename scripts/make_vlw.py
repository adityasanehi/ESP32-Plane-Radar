#!/usr/bin/env python3
"""Render a TTF into an anti-aliased VLW font (the format LovyanGFX/TFT_eSPI load).

  pip install freetype-py
  python3 scripts/make_vlw.py Font.ttf PIXEL_SIZE out.vlw [--latin1]

Glyphs: printable ASCII, plus U+00A0-U+00FF with --latin1 (accented city names).
"""
import struct
import sys

import freetype


def make_vlw(ttf, px, out, latin1=False):
    face = freetype.Face(ttf)
    face.set_pixel_sizes(0, px)
    codes = list(range(0x20, 0x7F)) + (list(range(0xA0, 0x100)) if latin1 else [])
    metrics, bitmaps = [], []
    for code in codes:
        if face.get_char_index(code) == 0:
            continue
        face.load_char(code, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
        g = face.glyph
        bm = g.bitmap
        rows = [bytes(bm.buffer[r * bm.pitch:r * bm.pitch + bm.width]) for r in range(bm.rows)]
        # unicode, height, width, xAdvance, dY (top above baseline), dX, padding
        metrics.append((code, bm.rows, bm.width, g.advance.x >> 6, g.bitmap_top, g.bitmap_left, 0))
        bitmaps.append(b"".join(rows))
    ascent = face.size.ascender >> 6
    descent = -(face.size.descender >> 6)
    with open(out, "wb") as f:
        f.write(struct.pack(">6i", len(metrics), 11, px, 0, ascent, descent))
        for m in metrics:
            f.write(struct.pack(">7i", *m))
        for b in bitmaps:
            f.write(b)


if __name__ == "__main__":
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    make_vlw(a[0], int(a[1]), a[2], latin1="--latin1" in sys.argv)
