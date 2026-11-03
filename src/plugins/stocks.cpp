// Page actions : une ligne par titre choisi dans le back office (cours, variation du jour, et sa
// valeur si une quantité est saisie, sa plus-value si le prix d'achat l'est aussi), puis le total
// du portefeuille (stocks.h)
#include "fonts/font_sans24.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans40.h"
#include "plugin.h"
#include "stocks.h"
#include "text.h"

#define HEAD_H      34   // en-têtes des colonnes
#define ROW_H       58
#define TOTAL_H     86   // filet, puis le total
#define PRICE_W     180  // le cours, à gauche de son bord droit : "1 234,56 €" fait 163 pixels
#define GAP         8    // entre deux colonnes

static uint32_t shown;  // version des cours affichés, 0 = rien

// Titres affichés à la fois ; au-delà, la page les fait défiler par groupes
#define GROUP       6
#define GROUP_MS    8000  // durée d'un groupe à l'écran ; la page garde l'écran jusqu'au dernier

static uint8_t group;         // groupe à l'écran
static uint32_t group_since;  // depuis quand
static uint8_t groups = 1;    // groupes à faire défiler
static bool seen_all;         // tous les groupes ont été montrés depuis l'arrivée de la page

static void draw() {
  static Stocks &s = *(Stocks *)heap_caps_malloc(sizeof(Stocks), MALLOC_CAP_SPIRAM);  // seule la tâche de l'écran passe ici
  stocksGet(s);
  bool wallet = false, costs = false;
  for (uint8_t i = 0; i < s.count; i++) {
    wallet |= s.stocks[i].quantity > 0;
    costs |= s.stocks[i].quantity > 0 && s.stocks[i].cost > 0;
  }

  // Totaux du portefeuille, sur tous les titres, et ordre d'affichage : du plus gros au plus petit
  double total = 0, day_from = 0, day_to = 0, cost_from = 0, cost_to = 0;
  static double *value = (double *)heap_caps_malloc(STOCKS_MAX * sizeof(double), MALLOC_CAP_SPIRAM);
  static uint8_t *order = (uint8_t *)heap_caps_malloc(STOCKS_MAX, MALLOC_CAP_SPIRAM);
  for (uint8_t i = 0; i < s.count; i++) {
    const Stock &st = s.stocks[i];
    value[i] = st.quantity * s.price[i];
    order[i] = i;
    total += value[i];
    if (s.previous[i] > 0) day_from += st.quantity * s.previous[i], day_to += value[i];
    if (st.cost > 0 && s.price[i] > 0) cost_from += st.quantity * st.cost, cost_to += value[i];
  }
  for (uint8_t i = 1; i < s.count; i++) {
    for (uint8_t j = i; j > 0 && value[order[j]] > value[order[j - 1]]; j--) std::swap(order[j], order[j - 1]);
  }
  groups = max<uint8_t>(1, (s.count + GROUP - 1) / GROUP);
  if (group >= groups) group = 0;
  // Groupes de taille égale, à un titre près : 19 titres font 5, 5, 5 et 4, pas 6, 6, 6 et 1
  uint8_t rows = (s.count + groups - 1) / groups;
  uint8_t first = group * s.count / groups, last = (group + 1) * s.count / groups;

  // Bord droit de chaque colonne : cours, jour, valeur, plus-value. Les colonnes absentes laissent
  // leur place aux autres.
  static const int16_t FULL[] = {470, 600, 830, LCD_WIDTH - MARGIN_X}, NO_COST[] = {560, 720, LCD_WIDTH - MARGIN_X, 0},
                       PRICES[] = {700, LCD_WIDTH - MARGIN_X, 0, 0};
  static const char *const HEADS[] = {"Cours", "Jour", "Valeur", "Plus-value"};
  const int16_t *col = costs ? FULL : wallet ? NO_COST : PRICES;
  uint8_t cols = costs ? 4 : wallet ? 3 : 2;
  auto left = [&](uint8_t k) -> int16_t { return k == 0 ? col[0] - PRICE_W : col[k - 1] + GAP; };
  auto width = [&](uint8_t k) -> int16_t { return col[k] - left(k); };
  int16_t name_w = left(0) - GAP - MARGIN_X;

  // Même hauteur pour tous les groupes : le total ne bouge pas d'un groupe à l'autre
  int16_t y = PAGE_Y + (PAGE_HEIGHT - HEAD_H - rows * ROW_H - (wallet ? TOTAL_H : 0)) / 2;
  gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);
  // Jusqu'à trois couleurs de texte par ligne (blanc, vert, rouge) : antialiasing réduit pour
  // rester dans les 16 du pilote (gfx.h)
  gfxSetCoarse(true);
  char text[32];
  if (groups > 1) {
    snprintf(text, sizeof(text), "%u / %u", group + 1, groups);
    gfxTextBox(MARGIN_X, y, name_w, text, font_sans24, COLOR_DIM, COLOR_BG);
  }
  for (uint8_t k = 0; k < cols; k++) gfxTextBox(left(k), y, width(k), HEADS[k], font_sans24, COLOR_DIM, COLOR_BG, GFX_RIGHT);
  int16_t row_y = y + HEAD_H;
  y = row_y + rows * ROW_H;

  for (uint8_t n = first; n < last; n++, row_y += ROW_H) {
    uint8_t i = order[n];
    const Stock &st = s.stocks[i];
    const char *name = st.name[0] ? st.name : s.title[i][0] ? s.title[i] : st.isin;
    gfxTextBox(MARGIN_X, row_y, name_w, name, font_sans32, COLOR_TEXT, COLOR_BG);
    double price = s.price[i], previous = s.previous[i];
    if (price <= 0) continue;  // titre inconnu de la bourse
    formatEuros(price, text, sizeof(text));
    gfxTextBox(left(0), row_y, width(0), text, font_sans32, COLOR_TEXT, COLOR_BG, GFX_RIGHT);
    if (previous > 0) pluginDrawChange(left(1), row_y, width(1), (price / previous - 1) * 100, font_sans32);
    if (st.quantity <= 0) continue;
    formatEuros(value[i], text, sizeof(text));
    gfxTextBox(left(2), row_y, width(2), text, font_sans32, COLOR_TEXT, COLOR_BG, GFX_RIGHT);
    if (st.cost > 0) pluginDrawChange(left(3), row_y, width(3), (price / st.cost - 1) * 100, font_sans32);
  }
  if (wallet) {
    int16_t dy = (font_sans40.line_height - font_sans32.line_height) / 2 + 2;
    gfxFillRect(MARGIN_X, y + 6, CONTENT_W, 2, COLOR_RULE);
    gfxTextBox(MARGIN_X, y + 24, name_w, "Portefeuille", font_sans40, COLOR_TEXT, COLOR_BG);
    if (day_from > 0) pluginDrawChange(left(1), y + 24 + dy, width(1), (day_to / day_from - 1) * 100, font_sans32);
    formatEuros(total, text, sizeof(text));
    gfxTextBox(left(2), y + 24, width(2), text, font_sans40, COLOR_TEXT, COLOR_BG, GFX_RIGHT);
    if (cost_from > 0) pluginDrawChange(left(3), y + 24 + dy, width(3), (cost_to / cost_from - 1) * 100, font_sans32);
  }
  gfxSetCoarse(false);
}

static bool stocksActive() {
  return stocksVersion() != 0;
}

static void stocksUpdate() {
  uint32_t version = stocksVersion();
  if (version == 0) return;  // la page va céder la place
  bool next = groups > 1 && millis() - group_since >= GROUP_MS;
  if (version == shown && !next) return;
  // Groupe suivant ; draw() revient au premier après le dernier
  if (next) group++, group_since = millis();
  shown = version;
  draw();
}

// Tour prolongé tant que le dernier groupe n'a pas été montré en entier. Appelé avant
// stocksUpdate(), qui reviendrait au premier groupe : une fois tout montré, la page ne retient plus
// l'écran, même si elle recommence un tour.
static bool stocksBusy() {
  if (group == groups - 1 && millis() - group_since >= GROUP_MS) seen_all = true;
  return !seen_all;
}

static void stocksShow() {
  shown = 0;
  group = 0;
  group_since = millis();
  seen_all = false;
  stocksUpdate();
}

extern const Plugin stocks_plugin = {"actions", false,   stocksBegin, stocksActive, stocksShow,
                                     stocksUpdate, nullptr, false,       false,        nullptr, stocksBusy};
