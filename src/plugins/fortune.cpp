// Page fortune : ce que valent les actions (stocks.h) et les cryptos (crypto.h) dont une quantité
// est saisie dans le back office, chacune avec ce qu'elle a gagné ou perdu, en euros et en pour
// cent, puis leur total
#include "crypto.h"
#include "fonts/font_sans24.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans40.h"
#include "fonts/font_sans48.h"
#include "plugin.h"
#include "stocks.h"
#include "text.h"

#define ROW_H       80
#define TOTAL_H     110  // filet, puis le total
#define NOTE_H      60   // la note sous le total
#define LABEL_X     112
#define LABEL_W     240
#define VALUE_R     600  // bord droit des montants : "123 456 €" fait 216 pixels en grand
#define GAIN_R      790  // bord droit des gains ou pertes en euros
#define CHANGE_R    912  // bord droit des variations en pour cent ; le tableau est centré

static uint32_t shown_stocks, shown_crypto;  // versions affichées

// Valeur d'un portefeuille et ce qu'elle était au début de la période de variation
struct Holding {
  double value, before;  // before = 0 : variation inconnue
};

static Holding shares() {
  static Stocks &s = *(Stocks *)heap_caps_malloc(sizeof(Stocks), MALLOC_CAP_SPIRAM);  // seule la tâche de l'écran passe ici
  Holding h = {0, 0};
  if (!stocksVersion()) return h;
  stocksGet(s);
  bool known = true;
  for (uint8_t i = 0; i < s.count; i++) {
    double q = s.stocks[i].quantity;
    h.value += q * s.price[i];
    h.before += q * s.previous[i];
    known &= q <= 0 || s.previous[i] > 0;
  }
  if (!known) h.before = 0;
  return h;
}

static Holding coins() {
  static Crypto &c = *(Crypto *)heap_caps_malloc(sizeof(Crypto), MALLOC_CAP_SPIRAM);  // seule la tâche de l'écran passe ici
  Holding h = {0, 0};
  if (!cryptoVersion()) return h;
  cryptoGet(c);
  bool known = true;
  for (uint8_t i = 0; i < c.count; i++) {
    double value = c.coins[i].quantity * c.price[i];
    float change = c.change[i][1];  // sur 24 heures
    h.value += value;
    if (isnan(change)) known &= value <= 0;
    else h.before += value / (1 + change / 100);
  }
  if (!known) h.before = 0;
  return h;
}

// Gain ou perte en euros, de la même couleur que la variation en pour cent : "+123,45 €", "-1 234 €"
static void gain(int16_t x, int16_t y, int16_t w, double delta) {
  char text[32];
  bool flat = fabs(delta) < 0.005;
  text[0] = delta < 0 ? '-' : '+';
  // Sous 1 €, deux décimales aussi : formatEuros() en met quatre, pour les cours des petites cryptos
  if (fabs(delta) >= 0.995) formatEuros(fabs(delta), text + !flat, sizeof(text) - 1);
  else snprintf(text + !flat, sizeof(text) - 1, "0,%02d €", flat ? 0 : (int)lround(fabs(delta) * 100) % 100);
  gfxTextBox(x, y, w, text, font_sans32, flat ? COLOR_TEXT : delta < 0 ? COLOR_DOWN : COLOR_UP, COLOR_BG, GFX_RIGHT);
}

static void line(int16_t y, const char *label, const Holding &h, const GfxFont &font) {
  char text[32];
  int16_t dy = (font.line_height - font_sans32.line_height) / 2 + 2;
  gfxTextBox(LABEL_X, y, LABEL_W, label, font, COLOR_TEXT, COLOR_BG);
  formatEuros(h.value, text, sizeof(text));
  gfxTextBox(LABEL_X + LABEL_W, y, VALUE_R - LABEL_X - LABEL_W, text, font, COLOR_TEXT, COLOR_BG, GFX_RIGHT);
  if (h.before <= 0) return;
  gain(VALUE_R + 8, y + dy, GAIN_R - VALUE_R - 8, h.value - h.before);
  pluginDrawChange(GAIN_R + 8, y + dy, CHANGE_R - GAIN_R - 8, (h.value / h.before - 1) * 100, font_sans32);
}

static void draw() {
  Holding a = shares(), c = coins();
  bool both = a.value > 0 && c.value > 0;
  uint8_t rows = (a.value > 0) + (c.value > 0);
  int16_t y = PAGE_Y + (PAGE_HEIGHT - rows * ROW_H - (both ? TOTAL_H : 0) - NOTE_H) / 2;
  gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);
  // Trois couleurs de texte par ligne (blanc, vert, rouge) : antialiasing réduit (gfx.h)
  gfxSetCoarse(true);
  if (a.value > 0) line(y, "Actions", a, font_sans40), y += ROW_H;
  if (c.value > 0) line(y, "Crypto", c, font_sans40), y += ROW_H;
  if (both) {
    gfxFillRect(LABEL_X, y + 6, CHANGE_R - LABEL_X, 2, COLOR_RULE);
    Holding total = {a.value + c.value, a.before > 0 && c.before > 0 ? a.before + c.before : 0};
    line(y + 30, "Total", total, font_sans48);
    y += TOTAL_H;
  }
  const char *note = both ? "Actions : variation du jour. Crypto : sur 24 heures."
                     : a.value > 0 ? "Variation du jour" : "Variation sur 24 heures";
  gfxTextBox(LABEL_X, y + 24, CHANGE_R - LABEL_X, note, font_sans24, COLOR_DIM, COLOR_BG, GFX_CENTER);
  gfxSetCoarse(false);
}

// Quelque chose à montrer : au moins une quantité saisie, avec ses cours. Demandé à chaque image :
// recalculé seulement quand les cours changent.
static bool fortuneActive() {
  static uint32_t seen_stocks = UINT32_MAX, seen_crypto = UINT32_MAX;
  static bool active;
  uint32_t s = stocksVersion(), c = cryptoVersion();
  if (s != seen_stocks || c != seen_crypto) {
    seen_stocks = s, seen_crypto = c;
    active = shares().value > 0 || coins().value > 0;
  }
  return active;
}

static void fortuneUpdate() {
  uint32_t s = stocksVersion(), c = cryptoVersion();
  if (s == shown_stocks && c == shown_crypto) return;
  shown_stocks = s, shown_crypto = c;
  if (s || c) draw();  // sinon, la page va céder la place
}

static void fortuneShow() {
  shown_stocks = shown_crypto = UINT32_MAX;
  fortuneUpdate();
}

extern const Plugin fortune_plugin = {"fortune", false, nullptr, fortuneActive, fortuneShow, fortuneUpdate, nullptr};
