// Page Wikipédia : l'article du jour de la rubrique « Lumière sur » (wiki.h), mis en page comme la
// page Sonos : l'image à gauche, le titre et le début de l'article à droite
#include "fonts/font_sans24.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans40.h"
#include "fonts/font_sans48.h"
#include "plugin.h"
#include "wiki.h"

#define ART_X     MARGIN_X
#define ART_Y     130
#define ART_STEP  20  // lignes d'image dessinées par image d'écran, comme la pochette Sonos
#define HEAD_Y    128
#define TITLE_Y   176
#define TITLE_H   67  // hauteur de ligne de la plus grande police du titre
#define TEXT_Y    262
#define TEXT_END  (LCD_HEIGHT - 40)
#define LINE_H    36

static uint32_t shown;        // version de l'article affiché, 0 = rien
static WikiArticle article;   // article affiché
static int16_t image_x, image_y, image_row;  // image : position, et prochaine ligne à dessiner

// Le titre prend la plus grande police dans laquelle il tient en entier ; sinon la plus petite, coupée
static void drawTitle(int16_t x, int16_t w, const char *title) {
  const GfxFont *font = &font_sans32;
  for (const GfxFont *f : {&font_sans48, &font_sans40}) {
    if (gfxTextWidth(title, *f) <= w) {
      font = f;
      break;
    }
  }
  gfxTextBox(x, TITLE_Y + (TITLE_H - font->line_height) / 2, w, title, *font, COLOR_TEXT, COLOR_BG);
}

// Texte sur plusieurs lignes, coupé entre les mots ; la dernière ligne finit par "..." s'il en reste
static void drawParagraph(int16_t x, int16_t y, int16_t w, const char *text, uint8_t lines) {
  static char line[256];
  const char *p = text;
  for (uint8_t i = 0; i < lines && *p; i++, y += LINE_H) {
    while (*p == ' ') p++;
    if (i == lines - 1) {
      gfxTextBox(x, y, w, p, font_sans24, COLOR_TEXT, COLOR_BG);
      break;
    }
    // Autant de mots que la ligne en tient ; un mot plus long que la ligne est coupé
    size_t fit = 0;
    for (size_t end = 0; p[end];) {
      size_t next = end;
      while (p[next] == ' ') next++;
      while (p[next] && p[next] != ' ') next++;
      size_t n = min(next, sizeof(line) - 1);
      memcpy(line, p, n);
      line[n] = 0;
      if (gfxTextWidth(line, font_sans24) > w) break;
      fit = end = next;
      if (!p[next]) break;
    }
    if (fit == 0) fit = strcspn(p, " ");
    memcpy(line, p, min(fit, sizeof(line) - 1));
    line[min(fit, sizeof(line) - 1)] = 0;
    gfxTextBox(x, y, w, line, font_sans24, COLOR_TEXT, COLOR_BG);
    p += fit;
  }
}

static void draw() {
  wikiGet(article);
  gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);
  bool image = article.image != nullptr;
  int16_t x = image ? ART_X + WIKI_IMAGE_MAX + 40 : MARGIN_X, w = LCD_WIDTH - MARGIN_X - x;
  gfxTextBox(x, HEAD_Y, w, "Wikipédia · Lumière sur", font_sans32, COLOR_DIM, COLOR_BG);
  drawTitle(x, w, article.title);
  drawParagraph(x, TEXT_Y, w, article.text, (TEXT_END - TEXT_Y) / LINE_H);
  // L'image, centrée dans son carré, part à l'écran par tranches (drawImageSlice)
  if (image) {
    image_x = ART_X + (WIKI_IMAGE_MAX - article.image_w) / 2;
    image_y = ART_Y + (WIKI_IMAGE_MAX - article.image_h) / 2;
    lcdImageArea(image_x, image_y, article.image_w, article.image_h);
  } else {
    lcdImageArea(0, 0, 0, 0);
  }
  image_row = 0;
}

// Ecrire toute l'image d'un coup en PSRAM pendant que l'écran la relit pourrait faire rater une
// image : elle est dessinée par tranches, une par image d'écran (comme la pochette Sonos)
static void drawImageSlice() {
  if (!article.image || image_row >= article.image_h) return;
  int16_t n = lcdFade() == 0 ? article.image_h - image_row : min<int16_t>(ART_STEP, article.image_h - image_row);
  gfxBlit(image_x, image_y + image_row, article.image_w, n, article.image + image_row * article.image_w);
  image_row += n;
}

static bool wikiActive() {
  return wikiVersion() != 0;
}

static void wikiUpdate() {
  uint32_t version = wikiVersion();
  if (version == 0) return;  // la page va céder la place
  if (version != shown) {
    shown = version;
    draw();
  }
  drawImageSlice();
}

static void wikiShow() {
  shown = 0;
  wikiUpdate();
}

// L'image est effacée par tranches avant de rendre l'écran, pour la même raison qu'à l'affichage
static void wikiHide() {
  if (article.image) {
    for (int16_t y = 0; y < article.image_h && lcdFade() != 0; y += ART_STEP) {
      gfxFillRect(image_x, image_y + y, article.image_w, ART_STEP, COLOR_BG);
      lcdPresent();
    }
  }
  lcdImageArea(0, 0, 0, 0);
  lcdPresent();
}

extern const Plugin wiki_plugin = {"wikipedia", false, wikiBegin, wikiActive, wikiShow, wikiUpdate, wikiHide,
                                   false,       false, "Wikipédia"};
