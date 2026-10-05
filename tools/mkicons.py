#!/usr/bin/env python3
"""Dessine des pictogrammes et les écrit comme une police pour gfxText() (4 bits/pixel).

Exemples :
  mkicons.py 64 font_weather64 > include/fonts/font_weather64.h
  mkicons.py 28 font_space28 --set space > include/fonts/font_space28.h

Un caractère par pictogramme, de 'a' à 'j' : voir ICONS en bas. Un pictogramme peut avoir une
seconde couche, à dessiner par-dessus dans une autre couleur (l'astre, la pluie, l'éclair) : elle
est 10 caractères plus loin, de 'k' à 't'. L'antialiasing n'a que 5 niveaux, pour que trois
couleurs tiennent dans les 16 que le pilote d'écran garde par ligne.

Les dessins sont faits ici, au trait, sans rien reprendre d'une police d'icônes. Nécessite Pillow
(pip install pillow).
"""
import argparse
import math
from PIL import Image, ImageChops, ImageDraw

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("size", type=int, help="côté d'un pictogramme en pixels")
parser.add_argument("name", help="nom de la variable C")
parser.add_argument("--set", default="weather", choices=["weather", "space"], help="jeu de pictogrammes : météo (par défaut) ou espace")
parser.add_argument("--preview", help="enregistre aussi une planche PNG de tous les pictogrammes")
args = parser.parse_args()

# Les dessins sont décrits dans une case de 64 x 64, tracés 8 fois plus grand puis réduits :
# c'est la réduction qui donne l'antialiasing
SS = 8
K = args.size / 64 * SS
STROKE = 5.0  # épaisseur du trait, dans la case de 64
LEVELS = 5    # niveaux d'antialiasing


def canvas():
    return Image.new("L", (args.size * SS, args.size * SS), 0)


def disc(img, x, y, r, fill=255):
    if r > 0:
        ImageDraw.Draw(img).ellipse([(x - r) * K, (y - r) * K, (x + r) * K, (y + r) * K], fill=fill)


def line(img, x0, y0, x1, y1, fill=255):
    """Trait à bouts ronds."""
    ImageDraw.Draw(img).line([x0 * K, y0 * K, x1 * K, y1 * K], fill=fill, width=round(STROKE * K))
    disc(img, x0, y0, STROKE / 2, fill)
    disc(img, x1, y1, STROKE / 2, fill)


def outline(shape):
    """Contour d'une forme pleine : la forme grossie d'un demi-trait, moins la forme rétrécie d'autant."""
    outer, inner = canvas(), canvas()
    shape(outer, STROKE / 2)
    shape(inner, -STROKE / 2)
    return ImageChops.subtract(outer, inner)


def cloud(ox=0.0, oy=0.0, k=1.0):
    def shape(img, grow, fill=255):
        for x, y, r in ((20, 36, 9), (46, 36, 9), (30, 27, 13), (41, 29, 9)):
            disc(img, ox + x * k, oy + y * k, r * k + grow, fill)
        ImageDraw.Draw(img).rectangle(
            [(ox + 20 * k) * K, (oy + 27 * k - grow) * K, (ox + 46 * k) * K, (oy + 45 * k + grow) * K], fill=fill)
    return shape


def sun(cx, cy, r, ray0, ray1, rays=range(8)):
    img = outline(lambda im, grow: disc(im, cx, cy, r + grow))
    for i in rays:
        a = i * math.pi / 4
        line(img, cx + ray0 * math.cos(a), cy + ray0 * math.sin(a), cx + ray1 * math.cos(a), cy + ray1 * math.sin(a))
    return img


def moon(cx, cy, r):
    """Croissant : un disque entamé par un autre, décalé en haut à droite."""
    def shape(img, grow):
        disc(img, cx, cy, r + grow)
        disc(img, cx + r * 0.62, cy - r * 0.38, r * 0.86 - grow, fill=0)
    return outline(shape)


def behind_cloud(back, c):
    """Astre à moitié caché, en seconde couche : le nuage efface ce qui est derrière lui, avec un peu de marge."""
    c(back, STROKE / 2 + 2.5, fill=0)
    return outline(c), back


def with_cloud(draw_below, own_layer=False):
    """Nuage et ce qu'il y a dessous, sur la seconde couche si c'est d'une autre couleur."""
    img, below = outline(cloud(oy=-8)), canvas()
    draw_below(below)
    return (img, below) if own_layer else ImageChops.lighter(img, below)


def drops(length):
    def draw(img):
        for x in (21, 32, 43):
            line(img, x + 1.5, 47, x + 1.5 - length * 0.33, 47 + length)
    return draw


def flakes(img):
    for x, y in ((20, 49), (32, 55), (44, 49)):
        disc(img, x, y, 3.2)


def bolt(img):
    pts = [(35, 42), (26, 54), (32, 54), (29, 63), (40, 50), (34, 50), (38, 42)]
    ImageDraw.Draw(img).polygon([(x * K, y * K) for x, y in pts], fill=255)


def mist(img):
    line(img, 14, 48, 50, 48)
    line(img, 22, 57, 42, 57)


def astronaut():
    """Casque rond à visière pleine, posé sur un col."""
    img = outline(lambda im, grow: disc(im, 32, 27, 21 + grow))
    ImageDraw.Draw(img).rounded_rectangle([20 * K, 19 * K, 44 * K, 35 * K], radius=8 * K, fill=255)
    ImageDraw.Draw(img).rounded_rectangle([17 * K, 52 * K, 47 * K, 62 * K], radius=4 * K, fill=255)
    return img


small_cloud = cloud(ox=9, oy=14, k=0.88)
SPACE = [("a", "astronaute", astronaut)]
WEATHER = [
    ("a", "soleil", lambda: sun(32, 32, 11, 18, 24)),
    ("b", "lune", lambda: moon(29, 33, 19)),
    ("c", "soleil et nuage", lambda: behind_cloud(sun(23, 23, 8, 13.5, 18, range(3, 8)), small_cloud)),
    ("d", "lune et nuage", lambda: behind_cloud(moon(22, 23, 14), small_cloud)),
    ("e", "nuage", lambda: outline(cloud(oy=2))),
    ("f", "brouillard", lambda: with_cloud(mist)),
    ("g", "bruine", lambda: with_cloud(drops(4), True)),
    ("h", "pluie", lambda: with_cloud(drops(9), True)),
    ("i", "neige", lambda: with_cloud(flakes)),
    ("j", "orage", lambda: with_cloud(bolt, True)),
]
ICONS = SPACE if args.set == "space" else WEATHER

bitmap = bytearray()
glyphs = []
sheet = Image.new("L", (args.size * len(ICONS), args.size), 0)
drawn = [draw() for _, _, draw in ICONS]
drawn = [d if isinstance(d, tuple) else (d, canvas()) for d in drawn]
for layer in (0, 1):
    for i, layers in enumerate(drawn):
        img = layers[layer].reduce(SS)
        sheet.paste(ImageChops.lighter(sheet.crop((i * args.size, 0, (i + 1) * args.size, args.size)), img), (i * args.size, 0))
        box = img.getbbox()
        if not box:
            glyphs.append((0, 0, 0, 0, 0, args.size))
            continue
        x0, y0, x1, y1 = box
        px = img.crop(box).load()
        offset = len(bitmap)
        for y in range(y1 - y0):
            row = [(px[x, y] * LEVELS + 127) // 255 * (15 // LEVELS) for x in range(x1 - x0)]
            if len(row) & 1:
                row.append(0)
            bitmap.extend(row[k] << 4 | row[k + 1] for k in range(0, len(row), 2))
        glyphs.append((offset, x1 - x0, y1 - y0, x0, y0, args.size))
if args.preview:
    sheet.save(args.preview)

name = args.name
print(f"// Généré par tools/mkicons.py, {args.size} px. Ne pas modifier à la main.")
print("// " + ", ".join(f"'{c}' {label}" for c, label, _ in ICONS))
print(f"// Seconde couche de chacun, s'il en a une : {len(ICONS)} caractères plus loin")
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
print(f"inline const GfxFont {name} = {{{name}_bitmap, {name}_glyphs, {ord(ICONS[0][0])}, {ord(ICONS[-1][0]) + len(ICONS)}, {args.size}}};")
