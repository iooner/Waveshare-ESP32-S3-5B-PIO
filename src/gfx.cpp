#include "gfx.h"
#include "lcd.h"

#define NIGHT_RED  200  // rouge donné au blanc par la teinte de nuit complète

typedef uint32_t __attribute__((may_alias)) pixpair_t;

static uint8_t night = 0;  // teinte de nuit en place
static bool coarse = false;

void gfxSetCoarse(bool on) {
  coarse = on;
}

void gfxSetNight(uint8_t level) {
  night = min<uint8_t>(level, GFX_NIGHT_MAX);
}

uint8_t gfxNight() {
  return night;
}

// Couleur telle qu'elle est dessinée, teinte de nuit comprise
static uint16_t tinted(uint16_t color) {
  if (!night) return color;
  int32_t r = color >> 11 << 3, g = (color >> 5 & 0x3F) << 2, b = (color & 0x1F) << 3;
  int32_t red = ((r * 77 + g * 150 + b * 29) >> 8) * NIGHT_RED / 255;  // d'après la clarté de la couleur
  return RGB565(r + (red - r) * night / GFX_NIGHT_MAX, g - g * night / GFX_NIGHT_MAX, b - b * night / GFX_NIGHT_MAX);
}

// Remplit n pixels, deux par écriture
static inline void fill16(uint16_t *dst, uint16_t color, int32_t n) {
  if ((uintptr_t)dst & 2) {
    *dst++ = color;
    n--;
  }
  pixpair_t c32 = (uint32_t)color << 16 | color;
  pixpair_t *d32 = (pixpair_t *)dst;
  for (int32_t i = n >> 1; i > 0; i--) *d32++ = c32;
  if (n & 1) *(uint16_t *)d32 = color;
}

// Remplit un rectangle sans le signaler au pilote
static void fillRaw(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (!lcdClip(x, y, w, h)) return;
  uint16_t *row = lcd_fb + y * LCD_WIDTH + x;
  for (int16_t i = 0; i < h; i++, row += LCD_WIDTH) fill16(row, color, w);
}

// Mélange fg sur bg, alpha de 0 à 15. Les trois composantes sont calculées en une multiplication.
static inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t alpha) {
  uint32_t a = (alpha * 32 + 7) / 15;
  uint32_t f = (fg | (uint32_t)fg << 16) & 0x07E0F81F;
  uint32_t b = (bg | (uint32_t)bg << 16) & 0x07E0F81F;
  uint32_t r = (b + (((f - b) * a) >> 5)) & 0x07E0F81F;
  return r | r >> 16;
}

void gfxClear(uint16_t color) {
  gfxFillRect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

void gfxPixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
  lcd_fb[y * LCD_WIDTH + x] = tinted(color);
  lcdDirty(x, y, 1, 1);
}

void gfxFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  fillRaw(x, y, w, h, tinted(color));
  lcdDirty(x, y, w, h);
}

void gfxRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  gfxFillRect(x, y, w, 1, color);
  gfxFillRect(x, y + h - 1, w, 1, color);
  gfxFillRect(x, y, 1, h, color);
  gfxFillRect(x + w - 1, y, 1, h, color);
}

void gfxLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  if (y0 == y1) return gfxFillRect(min(x0, x1), y0, abs(x1 - x0) + 1, 1, color);
  if (x0 == x1) return gfxFillRect(x0, min(y0, y1), 1, abs(y1 - y0) + 1, color);

  lcdDirty(min(x0, x1), min(y0, y1), abs(x1 - x0) + 1, abs(y1 - y0) + 1);
  color = tinted(color);
  // Bresenham
  int16_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int16_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;
  for (;;) {
    if (x0 >= 0 && y0 >= 0 && x0 < LCD_WIDTH && y0 < LCD_HEIGHT) lcd_fb[y0 * LCD_WIDTH + x0] = color;
    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = 2 * err;
    if (e2 >= dy) err += dy, x0 += sx;
    if (e2 <= dx) err += dx, y0 += sy;
  }
}

void gfxBlit(int16_t x, int16_t y, int16_t w, int16_t h, const uint16_t *pixels) {
  int16_t cx = x, cy = y, cw = w, ch = h;
  if (!lcdClip(cx, cy, cw, ch)) return;
  const uint16_t *src = pixels + (cy - y) * w + (cx - x);
  uint16_t *dst = lcd_fb + cy * LCD_WIDTH + cx;
  for (int16_t i = 0; i < ch; i++, src += w, dst += LCD_WIDTH) {
    if (!night) memcpy(dst, src, cw * sizeof(uint16_t));
    else for (int16_t k = 0; k < cw; k++) dst[k] = tinted(src[k]);
  }
  lcdDirty(cx, cy, cw, ch);
}

// Lit un point de code UTF-8 et avance le pointeur. Caractère absent de la police : '?',
// ou rien si la police n'a pas de '?' (police réduite à quelques caractères).
static const GfxGlyph &nextGlyph(const char *&s, const GfxFont &font) {
  static const GfxGlyph none = {};
  uint8_t c = *s++;
  uint32_t cp = c;
  uint8_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (extra) cp = c & (0x3F >> extra);
  while (extra-- && (*s & 0xC0) == 0x80) cp = cp << 6 | (*s++ & 0x3F);
  if (cp == 0x20AC) cp = 0x80;  // l'euro est rangé à sa place de Windows-1252 (tools/mkfont.py)
  if (cp < font.first || cp > font.last) cp = '?';
  if (cp < font.first || cp > font.last) return none;
  return font.glyphs[cp - font.first];
}

int16_t gfxTextWidth(const char *text, const GfxFont &font) {
  int16_t w = 0;
  while (*text) w += nextGlyph(text, font).advance;
  return w;
}

int16_t gfxText(int16_t x, int16_t y, const char *text, const GfxFont &font, uint16_t color, int32_t bg) {
  int16_t width = gfxTextWidth(text, font);
  bool opaque = bg != GFX_TRANSPARENT;
  color = tinted(color);
  if (opaque) bg = tinted(bg);

  // Zone modifiée, signalée en une fois : la boîte du texte si elle est repeinte, plus les
  // glyphes qui en débordent
  int16_t dx0 = LCD_WIDTH, dy0 = LCD_HEIGHT, dx1 = 0, dy1 = 0;

  // Fond uni : les 16 niveaux d'antialiasing sont calculés une seule fois, sans relire l'écran
  uint16_t lut[16] = {};
  if (opaque) {
    fillRaw(x, y, width, font.line_height, bg);
    for (uint8_t a = 0; a < 16; a++) lut[a] = blend565(color, bg, a);
    dx0 = x, dy0 = y, dx1 = x + width, dy1 = y + font.line_height;
  }

  int16_t pen = x;
  while (*text) {
    const GfxGlyph &g = nextGlyph(text, font);
    int16_t gx = pen + g.ox, gy = y + g.oy;
    pen += g.advance;

    // Partie visible du glyphe
    int16_t c0 = max<int16_t>(0, -gx), c1 = min<int16_t>(g.w, LCD_WIDTH - gx);
    int16_t r0 = max<int16_t>(0, -gy), r1 = min<int16_t>(g.h, LCD_HEIGHT - gy);
    if (c0 >= c1 || r0 >= r1) continue;

    uint16_t stride = (g.w + 1) / 2;
    for (int16_t r = r0; r < r1; r++) {
      const uint8_t *src = font.bitmap + g.offset + r * stride;
      uint16_t *dst = lcd_fb + (gy + r) * LCD_WIDTH + gx;
      for (int16_t c = c0; c < c1; c++) {
        uint8_t a = c & 1 ? src[c >> 1] & 0x0F : src[c >> 1] >> 4;
        if (coarse) a = (a + 1) / 3 * 3;
        if (a == 0) continue;
        dst[c] = opaque ? lut[a] : a == 15 ? color : blend565(color, dst[c], a);
      }
    }
    dx0 = min<int16_t>(dx0, gx + c0), dy0 = min<int16_t>(dy0, gy + r0);
    dx1 = max<int16_t>(dx1, gx + c1), dy1 = max<int16_t>(dy1, gy + r1);
  }
  lcdDirty(dx0, dy0, dx1 - dx0, dy1 - dy0);
  return width;
}

void gfxTextBox(int16_t x, int16_t y, int16_t w, const char *text, const GfxFont &font, uint16_t color, uint16_t bg,
                GfxAlign align) {
  // Coupe le texte à la largeur de la boîte, en gardant la place des points de suspension
  char fitted[160];
  int16_t dots = gfxTextWidth("...", font), width = 0;
  size_t keep = 0;  // octets qui tiennent avec les points de suspension
  const char *p = text;
  while (*p) {
    width += nextGlyph(p, font).advance;
    if (width + dots <= w) keep = p - text;
    if (width > w || (size_t)(p - text) >= sizeof(fitted) - 4) {
      memcpy(fitted, text, keep);
      strcpy(fitted + keep, "...");
      text = fitted;
      break;
    }
  }

  int16_t tw = gfxTextWidth(text, font);
  int16_t tx = align == GFX_LEFT ? x : align == GFX_RIGHT ? x + w - tw : x + (w - tw) / 2;
  gfxFillRect(x, y, tx - x, font.line_height, bg);
  gfxText(tx, y, text, font, color, bg);
  gfxFillRect(tx + tw, y, x + w - tx - tw, font.line_height, bg);
}
