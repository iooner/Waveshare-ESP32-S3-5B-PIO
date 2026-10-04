// Plugin horloge : heure en grand, date en dessous. L'heure vient du réseau (net.h).
#include <time.h>
#include "fonts/font_clock.h"
#include "fonts/font_sans24.h"
#include "fonts/font_sans48.h"
#include "gfx.h"
#include "lcd.h"
#include "net.h"
#include "plugin.h"

#define TIME_Y    90
#define DATE_Y    360
#define STATUS_Y  500

static const char *const DAYS[] = {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"};
static const char *const MONTHS[] = {"janvier", "février", "mars",      "avril",   "mai",      "juin",
                                     "juillet", "août",    "septembre", "octobre", "novembre", "décembre"};

// Ce qui est actuellement à l'écran, pour ne redessiner que ce qui change
static char shown_time[9];
static char shown_date[48], shown_status[48];

// "HH:MM:SS" centré. Les chiffres ont tous la même largeur : chaque caractère garde sa place,
// on ne redessine que ceux qui changent (un seul chiffre la plupart des secondes).
static void drawTime(const char *text) {
  int16_t x = (LCD_WIDTH - gfxTextWidth(text, font_clock)) / 2;
  for (uint8_t i = 0; text[i]; i++) {
    char one[2] = {text[i], 0};
    if (text[i] != shown_time[i]) gfxText(x, TIME_Y, one, font_clock, COLOR_TEXT, COLOR_BG);
    x += gfxTextWidth(one, font_clock);
    shown_time[i] = text[i];
  }
}

// Ligne de texte centrée sur toute la largeur de l'écran
static void drawLine(int16_t y, const char *text, char *shown, size_t size, const GfxFont &font) {
  if (strcmp(text, shown) == 0) return;
  int16_t w = gfxTextWidth(text, font), x = (LCD_WIDTH - w) / 2;
  gfxFillRect(0, y, x, font.line_height, COLOR_BG);
  gfxFillRect(x + w, y, LCD_WIDTH - x - w, font.line_height, COLOR_BG);
  gfxText(x, y, text, font, COLOR_TEXT, COLOR_BG);
  strlcpy(shown, text, size);
}

static void clockUpdate() {
  if (!netTimeSynced()) {
    const char *status = netConnected() ? "Synchronisation de l'heure..." : "Connexion au Wi-Fi...";
    drawLine(STATUS_Y, status, shown_status, sizeof(shown_status), font_sans24);
    return;
  }

  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);

  char buf[48];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  drawTime(buf);
  snprintf(buf, sizeof(buf), "%s %d%s %s %d", DAYS[t.tm_wday], t.tm_mday, t.tm_mday == 1 ? "er" : "",
           MONTHS[t.tm_mon], t.tm_year + 1900);
  drawLine(DATE_Y, buf, shown_date, sizeof(shown_date), font_sans48);
  drawLine(STATUS_Y, "", shown_status, sizeof(shown_status), font_sans24);
}

static void clockShow() {
  gfxClear(COLOR_BG);
  memset(shown_time, 0, sizeof(shown_time));
  shown_date[0] = shown_status[0] = 0;
  clockUpdate();
}

extern const Plugin clock_plugin = {"horloge", 20, nullptr, clockShow, clockUpdate};
