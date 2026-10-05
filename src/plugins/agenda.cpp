// Page agenda : les prochains événements du Liège Hackerspace, la date et l'heure au-dessus de
// chaque titre (agenda.h)
#include <time.h>
#include "agenda.h"
#include "fonts/font_lghs56.h"
#include "fonts/font_sans24.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans40.h"
#include "plugin.h"
#include "text.h"

#define HEAD_H  72  // logo et nom
#define ROW_H   84  // la date en petit, le titre dessous
#define LOGO_W  52

static uint32_t shown;  // version de l'agenda affichée, 0 = rien
static int shown_day;   // jour de l'année de cet affichage : « aujourd'hui » et « demain » en dépendent

// "16 h" ou "9 h 30"
static void clockText(const struct tm &t, char *out, size_t cap) {
  if (t.tm_min) snprintf(out, cap, "%d h %02d", t.tm_hour, t.tm_min);
  else snprintf(out, cap, "%d h", t.tm_hour);
}

// Quand a lieu l'événement, dit comme on le dirait : "en ce moment, jusqu'à 22 h", "demain · 9 h 30",
// "mercredi 7 octobre · 16 h"
static void whenText(const IcalEvent &e, time_t now, char *out, size_t cap) {
  struct tm today, start, end;
  localtime_r(&now, &today);
  localtime_r(&e.start, &start);
  localtime_r(&e.end, &end);
  char hour[16];
  if (e.start <= now) {
    clockText(end, hour, sizeof(hour));
    if (e.all_day) strlcpy(out, "aujourd'hui", cap);
    else snprintf(out, cap, "en ce moment, jusqu'à %s", hour);
    return;
  }
  // Jours d'écart, comptés de date à date (midi : sans effet des changements d'heure)
  struct tm noon = today;
  noon.tm_hour = 12, noon.tm_min = noon.tm_sec = 0;
  struct tm start_noon = start;
  start_noon.tm_hour = 12, start_noon.tm_min = start_noon.tm_sec = 0;
  int days = lround(difftime(mktime(&start_noon), mktime(&noon)) / 86400);
  char day[40];
  if (days == 0) strlcpy(day, "aujourd'hui", sizeof(day));
  else if (days == 1) strlcpy(day, "demain", sizeof(day));
  else snprintf(day, sizeof(day), "%s %d%s %s", DAY_NAMES[start.tm_wday], start.tm_mday, start.tm_mday == 1 ? "er" : "",
                MONTH_NAMES[start.tm_mon]);
  clockText(start, hour, sizeof(hour));
  if (e.all_day) strlcpy(out, day, cap);
  else snprintf(out, cap, "%s · %s", day, hour);
}

static void draw() {
  Agenda a;
  agendaGet(a);
  time_t now = time(nullptr);
  int16_t y = PAGE_Y + (PAGE_HEIGHT - HEAD_H - a.count * ROW_H) / 2;

  gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);
  gfxText(MARGIN_X, y, "a", font_lghs56, COLOR_TEXT, COLOR_BG);
  gfxTextBox(MARGIN_X + LOGO_W + 20, y + (font_lghs56.line_height - font_sans40.line_height) / 2, CONTENT_W - LOGO_W - 20,
             "Liège Hackerspace", font_sans40, COLOR_TEXT, COLOR_BG);
  y += HEAD_H;
  for (uint8_t i = 0; i < a.count; i++, y += ROW_H) {
    char when[64];
    whenText(a.events[i], now, when, sizeof(when));
    gfxTextBox(MARGIN_X, y, CONTENT_W, when, font_sans24, COLOR_DIM, COLOR_BG);
    gfxTextBox(MARGIN_X, y + 30, CONTENT_W, a.events[i].title, font_sans32, COLOR_TEXT, COLOR_BG);
  }
}

static bool agendaActive() {
  return agendaVersion() != 0;
}

static void agendaUpdate() {
  uint32_t version = agendaVersion();
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  if (version == 0 || (version == shown && t.tm_yday == shown_day)) return;  // à 0, la page va céder la place
  shown = version;
  shown_day = t.tm_yday;
  draw();
}

static void agendaShow() {
  shown = 0;
  agendaUpdate();
}

extern const Plugin agenda_plugin = {"agenda", false, agendaBegin, agendaActive, agendaShow, agendaUpdate, nullptr};
