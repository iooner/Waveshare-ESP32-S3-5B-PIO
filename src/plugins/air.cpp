// Page air : les particules fines mesurées par le capteur de la maison (air.h), PM2,5 et PM10
// côte à côte, chacune avec son niveau par rapport aux repères de l'OMS
#include "air.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans40.h"
#include "fonts/font_sans48.h"
#include "fonts/font_space56.h"
#include "plugin.h"

#define HEAD_H      96
#define COL_W       (CONTENT_W / 2)
#define COLOR_GOOD  RGB565(60, 210, 110)
#define COLOR_FAIR  RGB565(255, 185, 30)
#define COLOR_BAD   RGB565(255, 90, 80)

static char shown[24];  // les deux valeurs affichées

// Une colonne, centrée : le nom de la mesure, sa valeur, son niveau. `guide` : valeur à ne pas dépasser
// en moyenne sur 24 heures selon l'OMS (2021).
static void column(int16_t x, int16_t y, const char *name, float value, float guide) {
  char text[24];
  snprintf(text, sizeof(text), "%.1f µg/m³", value);
  if (char *dot = strchr(text, '.')) *dot = ',';
  bool good = value <= guide, fair = value <= 2 * guide;
  gfxTextBox(x, y, COL_W, name, font_sans32, COLOR_DIM, COLOR_BG, GFX_CENTER);
  gfxTextBox(x, y + 52, COL_W, text, font_sans48, COLOR_TEXT, COLOR_BG, GFX_CENTER);
  gfxTextBox(x, y + 130, COL_W, good ? "Bon" : fair ? "Moyen" : "Mauvais", font_sans40,
             good ? COLOR_GOOD : fair ? COLOR_FAIR : COLOR_BAD, COLOR_BG, GFX_CENTER);
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
  column(MARGIN_X, y + HEAD_H, "Particules fines PM2,5", pm25, 15);
  column(MARGIN_X + COL_W, y + HEAD_H, "Particules PM10", pm10, 45);
  gfxSetCoarse(false);
}

static void airShow() {
  shown[0] = 0;
  airPageUpdate();
}

extern const Plugin air_plugin = {"air", false, nullptr, airActive, airShow, airPageUpdate, nullptr};
