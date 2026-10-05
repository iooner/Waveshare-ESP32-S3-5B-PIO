// Page crypto : une ligne par cryptomonnaie choisie dans le back office (cours, variations sur
// 1 heure, 24 heures et 7 jours, en pour cent, et sa valeur si une quantité est saisie), puis
// le total du portefeuille (crypto.h)
#include "crypto.h"
#include "fonts/font_sans24.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans40.h"
#include "fonts/font_sans48.h"
#include "plugin.h"

#define HEAD_H       34   // en-têtes des colonnes de variation
#define ROW_H        62
#define TOTAL_H      86   // filet, puis le total
#define SYMBOL_W     136
#define GAP          8    // entre deux colonnes
#define COLOR_UP     RGB565(60, 210, 110)
#define COLOR_DOWN   RGB565(255, 90, 80)
#define COLOR_RULE   RGB565(60, 60, 60)

static uint32_t shown;  // version des cours affichés, 0 = rien

// Montant en euros à la française : "76 658 €", "2 418,87 €", "0,1523 €"
static void euros(double amount, char *out, size_t cap) {
  char digits[24];
  int decimals = amount >= 10000 ? 0 : amount >= 1 ? 2 : 4;
  snprintf(digits, sizeof(digits), "%.*f", decimals, amount);
  char *dot = strchr(digits, '.');
  size_t whole = dot ? dot - digits : strlen(digits), n = 0;
  for (size_t i = 0; i < whole && n < cap - 1; i++) {
    if (i && (whole - i) % 3 == 0) out[n++] = ' ';  // espace des milliers
    out[n++] = digits[i];
  }
  out[n] = 0;
  if (dot) snprintf(out + n, cap - n, ",%s", dot + 1);
  strlcat(out, " €", cap);
}

static void draw() {
  Crypto c;
  cryptoGet(c);
  bool wallet = false;
  for (uint8_t i = 0; i < c.count; i++) wallet |= c.coins[i].quantity > 0;

  // Colonnes, alignées à droite : cours, variations sur 1 heure, 24 heures et 7 jours, puis valeur
  // détenue s'il y a un portefeuille. Sans lui, les variations s'étalent jusqu'au bord.
  static const int16_t WITH_WALLET[] = {420, 525, 625, 725}, WITHOUT[] = {480, 680, 828, LCD_WIDTH - MARGIN_X};
  static const char *const PERIODS[] = {"1 h", "24 h", "7 j"};
  const int16_t *col = wallet ? WITH_WALLET : WITHOUT;
  int16_t y = PAGE_Y + (PAGE_HEIGHT - HEAD_H - c.count * ROW_H - (wallet ? TOTAL_H : 0)) / 2;
  int16_t change_dy = (font_sans40.line_height - font_sans32.line_height) / 2 + 2;

  gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);
  // Jusqu'à trois couleurs de texte par ligne (blanc, vert, rouge) : antialiasing réduit pour
  // rester dans les 16 du pilote (gfx.h)
  gfxSetCoarse(true);
  for (uint8_t k = 0; k < 3; k++) {
    gfxTextBox(col[k] + GAP, y, col[k + 1] - col[k] - GAP, PERIODS[k], font_sans24, COLOR_DIM, COLOR_BG, GFX_RIGHT);
  }
  y += HEAD_H;
  double total = 0;
  char text[32];
  for (uint8_t i = 0; i < c.count; i++, y += ROW_H) {
    gfxTextBox(MARGIN_X, y, SYMBOL_W, c.coins[i].symbol, font_sans40, COLOR_TEXT, COLOR_BG);
    if (c.price[i] <= 0) continue;  // crypto inconnue du service
    euros(c.price[i], text, sizeof(text));
    gfxTextBox(MARGIN_X + SYMBOL_W, y, col[0] - MARGIN_X - SYMBOL_W, text, font_sans40, COLOR_TEXT, COLOR_BG, GFX_RIGHT);
    for (uint8_t k = 0; k < 3; k++) {
      float change = c.change[i][k];
      if (isnan(change)) continue;
      snprintf(text, sizeof(text), "%+.1f", change);
      if (char *dot = strchr(text, '.')) *dot = ',';
      gfxTextBox(col[k] + GAP, y + change_dy, col[k + 1] - col[k] - GAP, text, font_sans32, change < 0 ? COLOR_DOWN : COLOR_UP,
                 COLOR_BG, GFX_RIGHT);
    }
    if (c.coins[i].quantity <= 0) continue;
    double value = c.coins[i].quantity * c.price[i];
    total += value;
    euros(value, text, sizeof(text));
    gfxTextBox(col[3] + GAP, y, LCD_WIDTH - MARGIN_X - col[3] - GAP, text, font_sans40, COLOR_TEXT, COLOR_BG, GFX_RIGHT);
  }
  if (wallet) {
    gfxFillRect(MARGIN_X, y + 6, CONTENT_W, 2, COLOR_RULE);
    gfxTextBox(MARGIN_X, y + 24, 300, "Portefeuille", font_sans40, COLOR_DIM, COLOR_BG);
    euros(total, text, sizeof(text));
    gfxTextBox(MARGIN_X + 300, y + 18, CONTENT_W - 300, text, font_sans48, COLOR_TEXT, COLOR_BG, GFX_RIGHT);
  }
  gfxSetCoarse(false);
}

static bool cryptoActive() {
  return cryptoVersion() != 0;
}

static void cryptoUpdate() {
  uint32_t version = cryptoVersion();
  if (version == shown || version == 0) return;  // à 0, la page va céder la place
  shown = version;
  draw();
}

static void cryptoShow() {
  shown = 0;
  cryptoUpdate();
}

extern const Plugin crypto_plugin = {"crypto", false, cryptoBegin, cryptoActive, cryptoShow, cryptoUpdate, nullptr};
