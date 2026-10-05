#!/usr/bin/env python3
"""Convertit un logo d'une seule teinte en police d'un caractère ('a') pour gfxText() (4 bits/pixel).

Exemple :
  mklogo.py logo.png 56 font_lghs56 > include/fonts/font_lghs56.h

La forme est prise dans la transparence de l'image : elle sera dessinée dans la couleur passée à
gfxText(). Nécessite Pillow (pip install pillow).
"""
import argparse
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("image", help="logo en PNG, sur fond transparent")
parser.add_argument("height", type=int, help="hauteur en pixels")
parser.add_argument("name", help="nom de la variable C")
args = parser.parse_args()

alpha = Image.open(args.image).convert("RGBA").getchannel("A")
alpha = alpha.crop(alpha.getbbox())
width = max(1, round(alpha.width * args.height / alpha.height))
alpha = alpha.resize((width, args.height), Image.LANCZOS)
px = alpha.load()

bitmap = bytearray()
for y in range(args.height):
    row = [(px[x, y] * 15 + 127) // 255 for x in range(width)]
    if len(row) & 1:
        row.append(0)
    bitmap.extend(row[k] << 4 | row[k + 1] for k in range(0, len(row), 2))

name = args.name
print(f"// Généré par tools/mklogo.py depuis {args.image.split('/')[-1]}, {args.height} px de haut. Ne pas modifier à la main.")
print("#pragma once")
print('#include "gfx.h"')
print()
print(f"inline const uint8_t {name}_bitmap[] = {{")
for i in range(0, len(bitmap), 24):
    print("  " + ",".join(str(b) for b in bitmap[i:i + 24]) + ",")
print("};")
print()
print(f"inline const GfxGlyph {name}_glyphs[] = {{")
print("  {0, %d, %d, 0, 0, %d}," % (width, args.height, width))
print("};")
print()
print(f"inline const GfxFont {name} = {{{name}_bitmap, {name}_glyphs, 97, 97, {args.height}}};")
