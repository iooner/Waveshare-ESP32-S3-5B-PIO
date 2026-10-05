// Page air : les particules fines mesurées par le capteur de la maison (air.h), PM2,5 et PM10
// côte à côte, chacune avec son niveau sur les dix de l'indice belge de qualité de l'air (BelAQI)
#include "air.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans40.h"
#include "fonts/font_sans48.h"
#include "fonts/font_space56.h"
#include "plugin.h"

#define HEAD_H      96
#define COL_W       (CONTENT_W / 2)

// Les dix niveaux de l'indice belge BelAQI, du meilleur au pire. Les couleurs vont du bleu au
// rouge sombre comme les siennes, éclaircies aux deux bouts pour rester lisibles sur fond noir.
static const struct {
  const char *name;
  uint16_t color;
} LEVELS[] = {{"Excellent", RGB565(90, 150, 255)},   {"Très bon", RGB565(60, 185, 255)},  {"Bon", RGB565(50, 190, 80)},
              {"Assez bon", RGB565(110, 240, 90)},   {"Moyen", RGB565(250, 240, 70)},     {"Insuffisant", RGB565(255, 190, 40)},
              {"Assez mauvais", RGB565(255, 125, 30)}, {"Mauvais", RGB565(255, 70, 60)},  {"Très mauvais", RGB565(215, 45, 60)},
              {"Exécrable", RGB565(175, 50, 90)}};
#define LEVEL_COUNT  10
// Valeur en µg/m³ jusqu'à laquelle on reste à chaque niveau : seuils de l'indice horaire, fait pour
// juger l'air à un instant donné (https://wallonair.be/fr/en-savoir-plus/indice-de-la-qualite-de-l-air)
static const float PM25_STEPS[] = {3.5, 7.5, 10, 15, 20, 35, 50, 60, 75}, PM10_STEPS[] = {10, 20, 35, 45, 60, 80, 95, 110, 140};

static char shown[24];  // les deux valeurs affichées

// Une colonne, centrée : le nom de la mesure, sa valeur, son niveau
static void column(int16_t x, int16_t y, const char *name, float value, const float *steps) {
  char text[24];
  snprintf(text, sizeof(text), "%.1f µg/m³", value);
  if (char *dot = strchr(text, '.')) *dot = ',';
  uint8_t level = 0;
  while (level < LEVEL_COUNT - 1 && value > steps[level]) level++;
  char label[40];
  snprintf(label, sizeof(label), "%s · %u/10", LEVELS[level].name, level + 1);
  gfxTextBox(x, y, COL_W, name, font_sans32, COLOR_DIM, COLOR_BG, GFX_CENTER);
  gfxTextBox(x, y + 52, COL_W, text, font_sans48, COLOR_TEXT, COLOR_BG, GFX_CENTER);
  gfxTextBox(x, y + 130, COL_W, label, font_sans40, LEVELS[level].color, COLOR_BG, GFX_CENTER);
}

static bool airActive() {
  float pm25, pm10;
  return airGet(pm25, pm10);
}

static void airPageUpdate() {
  float pm25, pm10;
  if (!airGet(pm25, pm10)) return;  // la page va céder la place
  char both[24];
  snprintf(both, sizeof(both), "%.1f %.1f", pm25, pm10);
  if (strcmp(both, shown) == 0) return;
  strlcpy(shown, both, sizeof(shown));

  int16_t y = PAGE_Y + (PAGE_HEIGHT - HEAD_H - 190) / 2;
  gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);
  gfxText(MARGIN_X, y, "b", font_space56, COLOR_TEXT, COLOR_BG);
  gfxTextBox(MARGIN_X + 76, y + (56 - font_sans40.line_height) / 2, CONTENT_W - 76, "Qualité de l'air", font_sans40,
             COLOR_TEXT, COLOR_BG);
  // Les deux niveaux peuvent être de couleurs différentes sur la même ligne : antialiasing réduit (gfx.h)
  gfxSetCoarse(true);
  column(MARGIN_X, y + HEAD_H, "Particules fines PM2,5", pm25, PM25_STEPS);
  column(MARGIN_X + COL_W, y + HEAD_H, "Particules PM10", pm10, PM10_STEPS);
  gfxSetCoarse(false);
}

static void airShow() {
  shown[0] = 0;
  airPageUpdate();
}

extern const Plugin air_plugin = {"air", false, nullptr, airActive, airShow, airPageUpdate, nullptr};
