#!/usr/bin/env python3
"""Convertit une police TTF/OTF en en-tête C pour gfxText() (glyphes antialiasés 4 bits/pixel).

Exemples :
  mkfont.py OpenSans.ttf 24 font_sans24 --weight SemiBold > include/fonts/font_sans24.h
  mkfont.py OpenSans.ttf 200 font_clock --weight Bold --chars "0123456789:" > include/fonts/font_clock.h

Sans --chars, couvre l'ASCII et le Latin-1 (accents français), plus l'euro, rangé au code 0x80
comme dans Windows-1252 : gfxText() l'y cherche. Nécessite Pillow (pip install pillow).
"""
import argparse
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("font", help="fichier TTF ou OTF")
parser.add_argument("size", type=int, help="taille en pixels")
parser.add_argument("name", help="nom de la variable C")
parser.add_argument("--weight", help="graisse d'une police variable, ex. SemiBold")
parser.add_argument("--chars", help="ne garder que ces caractères : indispensable pour les grandes tailles")
args = parser.parse_args()

font = ImageFont.truetype(args.font, args.size)
if args.weight:
    font.set_variation_by_name(args.weight)

chars = set(args.chars) if args.chars else {chr(c) for c in range(32, 256) if not 127 <= c < 160} | {"\x80"}
ALIASES = {"\x80": "€"}  # caractère dessiné à la place de celui du code
first, last = min(map(ord, chars)), max(map(ord, chars))

ascent, descent = font.getmetrics()
line_h = ascent + descent
pad = args.size  # marge pour les glyphes qui débordent de leur case

bitmap = bytearray()
glyphs = []
for cp in range(first, last + 1):
    ch = ALIASES.get(chr(cp), chr(cp))
    adv = round(font.getlength(ch)) if chr(cp) in chars else 0
    img = Image.new("L", (adv + 2 * pad, line_h + 2 * pad), 0)
    if adv:
        ImageDraw.Draw(img).text((pad, pad), ch, font=font, fill=255)
    box = img.getbbox()
    if not box:
        glyphs.append((0, 0, 0, 0, 0, adv))
        continue
    x0, y0, x1, y1 = box
    px = img.crop(box).load()
    offset = len(bitmap)
    for y in range(y1 - y0):
        row = [(px[x, y] * 15 + 127) // 255 for x in range(x1 - x0)]
        if len(row) & 1:
            row.append(0)
        bitmap.extend(row[i] << 4 | row[i + 1] for i in range(0, len(row), 2))
    glyphs.append((offset, x1 - x0, y1 - y0, x0 - pad, y0 - pad, adv))

name = args.name
print(f"// Généré par tools/mkfont.py depuis {args.font.split('/')[-1]}, {args.size} px. Ne pas modifier à la main.")
print("#pragma once")
print('#include "gfx.h"')
print()
print(f"inline const uint8_t {name}_bitmap[] = {{")
for i in range(0, len(bitmap), 24):
    print("  " + ",".join(str(b) for b in bitmap[i:i + 24]) + ",")
print("};")
print()
print(f"inline const GfxGlyph {name}_glyphs[] = {{")
for g in glyphs:
    print("  {%d, %d, %d, %d, %d, %d}," % g)
print("};")
print()
print(f"inline const GfxFont {name} = {{{name}_bitmap, {name}_glyphs, {first}, {last}, {line_h}}};")
