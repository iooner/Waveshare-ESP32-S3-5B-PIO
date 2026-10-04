// Primitives de dessin dans le framebuffer du LCD. Tout est découpé aux bords de l'écran.
#pragma once
#include <Arduino.h>

#define RGB565(r, g, b)  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define COLOR_BLACK  0x0000
#define COLOR_WHITE  0xFFFF
#define COLOR_RED    0xF800
#define COLOR_GREEN  0x07E0
#define COLOR_BLUE   0x001F

// Fond de gfxText() : le texte est mélangé à ce qui est déjà dessiné (plus lent, relit la PSRAM)
#define GFX_TRANSPARENT  (-1)

// Polices générées par tools/mkfont.py : glyphes antialiasés, 4 bits par pixel
struct GfxGlyph {
  uint32_t offset;  // dans bitmap
  uint16_t w, h;
  int16_t ox, oy;   // position du glyphe par rapport au coin haut gauche de sa case
  uint16_t advance;
};

struct GfxFont {
  const uint8_t *bitmap;
  const GfxGlyph *glyphs;
  uint16_t first, last;  // points de code couverts
  uint16_t line_height;
};

// Teinte de nuit, de 0 (couleurs telles quelles) à GFX_NIGHT_MAX : chaque couleur dessinée glisse
// vers un rouge sombre de même clarté, comme l'écran de nuit d'un téléphone posé sur sa base.
// Elle vaut pour ce qui est dessiné ensuite : après un changement, la page est à redessiner.
#define GFX_NIGHT_MAX  16
void gfxSetNight(uint8_t level);
uint8_t gfxNight();

void gfxClear(uint16_t color);
void gfxPixel(int16_t x, int16_t y, uint16_t color);
void gfxFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void gfxRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
void gfxLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);

// Copie une image RGB565 de w x h pixels
void gfxBlit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels);

// Texte UTF-8, (x, y) = coin haut gauche. Avec une couleur de fond, toute la boîte du texte est
// repeinte : pas besoin d'effacer avant de réécrire. Renvoie la largeur dessinée en pixels.
int16_t gfxText(int16_t x, int16_t y, const char *text, const GfxFont &font, uint16_t color,
                int32_t bg = GFX_TRANSPARENT);
int16_t gfxTextWidth(const char *text, const GfxFont &font);

enum GfxAlign { GFX_LEFT, GFX_CENTER, GFX_RIGHT };

// Texte opaque aligné dans une boîte de largeur w. Le reste de la boîte est repeint avec le fond :
// un texte plus court efface le précédent. Un texte trop long est coupé et terminé par "...".
void gfxTextBox(int16_t x, int16_t y, int16_t w, const char *text, const GfxFont &font, uint16_t color, uint16_t bg,
                GfxAlign align = GFX_LEFT);
