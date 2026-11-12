#!/usr/bin/env python3
"""ui_atlas.py [out.bin]: the shell's smooth font (gfx_vk_present.c cv_s), baked from dm/third_party/roboto with PIL into
dm/third_party/roboto/ui_smooth.nxsf (the APK's assets/font/ui_smooth.bin, porter/pipeline.py ui_font_assets). Run it again after changing a face, size or
character set here; the result is committed, so ports need neither PIL nor the fonts.

Each face at the pixel sizes (the em) the shell draws it at, from 0.017 to 0.046 of a 400-1800 px window, rounded to a ratio of 1.4: drawing scales a glyph
down from the next larger size by area averaging. Regular and medium: printable ASCII, Latin-1 and a few more; bold italic (titles and the main button, always
in capitals): no lower case. The rest falls back to the pixel font. 8-bit coverage, packed in shelves.
Format (little endian): "NXSF", u32 version 1, u16 faces, u16 sizes, u16 atlas width, u16 atlas height, u16 sizes[sizes] (em px); per face and size: i16
ascent, i16 descent (below the baseline, positive), u32 first glyph, u32 glyphs (0: the face has no such size); then the glyphs, by code point within a set:
u32 code point, u16 x, y, w, h, i16 left, top (from the pen on the baseline; top < 0 is above it), u16 advance (1/64 px); then the coverage, width x height bytes"""
import struct, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
D = Path(__file__).resolve().parents[2] / 'third_party/roboto'
SIZES = [20, 28, 40, 56, 80]
TEXT = list(range(0x20, 0x7f)) + list(range(0xa0, 0x100)) + [0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2026, 0x20ac]
CAPS = [c for c in TEXT if not (0x61 <= c <= 0x7a or 0xdf <= c <= 0xff)]
FACES = [('Roboto-Regular-latin.ttf', [20, 28, 40, 56], TEXT), ('Roboto-Medium-latin.ttf', [20, 28, 40, 56], TEXT), ('Roboto-BoldItalic-latin.ttf', [20, 28, 40, 56, 80], CAPS)]
def main(out=str(D / 'ui_smooth.nxsf')):
    glyphs, metrics = [], []   # glyphs: (cp, img, left, top, adv)
    for name, sizes, chars in FACES:
        for px in SIZES:
            if px not in sizes: metrics.append((0, 0, len(glyphs), 0)); continue
            f = ImageFont.truetype(str(D / name), px); asc, desc = f.getmetrics(); first = len(glyphs); n = 0
            for cp in chars:
                ch = chr(cp)
                if f.getmask(ch).getbbox() is None and cp not in (0x20, 0xa0): continue   # not in the font
                l, t, r, b = f.getbbox(ch, anchor='ls'); w, h = max(0, r - l), max(0, b - t)
                img = Image.new('L', (w, h), 0)
                if w and h: ImageDraw.Draw(img).text((-l, -t), ch, font=f, fill=255, anchor='ls')
                glyphs.append((cp, img, l, t, int(round(f.getlength(ch) * 64)))); n += 1
            metrics.append((asc, desc, first, n))
    AW = 1024; order = sorted(range(len(glyphs)), key=lambda i: -glyphs[i][1].size[1]); pos = [None] * len(glyphs); x = y = sh = 0
    for i in order:   # shelves of glyphs of about one height, a texel between glyphs
        w, h = glyphs[i][1].size
        if x + w + 1 > AW: x, y, sh = 0, y + sh + 1, 0
        pos[i] = (x, y); x += w + 1; sh = max(sh, h)
    AH = y + sh + 1; atlas = Image.new('L', (AW, AH), 0)
    for (cp, img, l, t, adv), (gx, gy) in zip(glyphs, pos): atlas.paste(img, (gx, gy))
    b = bytearray(b'NXSF' + struct.pack('<IHHHH', 1, len(FACES), len(SIZES), AW, AH) + struct.pack(f'<{len(SIZES)}H', *SIZES))
    for asc, desc, first, n in metrics: b += struct.pack('<hhII', asc, desc, first, n)
    for (cp, img, l, t, adv), (gx, gy) in zip(glyphs, pos): b += struct.pack('<IHHHHhhH', cp, gx, gy, img.size[0], img.size[1], l, t, adv)
    b += atlas.tobytes(); Path(out).write_bytes(b)
    print(f'{out}: {len(FACES)} faces, {len(glyphs)} glyphs, atlas {AW}x{AH}, {len(b) / 1024:.0f} KB')
if __name__ == '__main__': main(*sys.argv[1:])
