// Pages agenda : les prochains événements, la date et l'heure au-dessus de chaque titre. Une page
// pour le Liège Hackerspace, une pour les agendas personnels donnés dans le back office (agenda.h)
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
#define ROWS    5   // événements par colonne
#define COL_GAP 40
#define LOGO_W  52

// Ce qui est à l'écran, pour chacune des deux pages
static uint32_t shown[AGENDA_LISTS];  // version de la liste affichée, 0 = rien
static int shown_day[AGENDA_LISTS];   // jour de l'année de cet affichage : « aujourd'hui » et « demain » en dépendent

// "16 h" ou "9 h 30"
static void clockText(const struct tm &t, char *out, size_t cap) {
  if (t.tm_min) snprintf(out, cap, "%d h %02d", t.tm_hour, t.tm_min);
  else snprintf(out, cap, "%d h", t.tm_hour);
}

// Quand a lieu l'événement, dit comme on le dirait : "en ce moment, jusqu'à 22 h", "demain · 9 h 30",
// "mercredi 7 octobre · 16 h" ; en bref, pour une colonne étroite : "mer. 7 oct. · 16 h"
static void whenText(const IcalEvent &e, time_t now, bool brief, char *out, size_t cap) {
  struct tm today, start, end;
  localtime_r(&now, &today);
  localtime_r(&e.start, &start);
  localtime_r(&e.end, &end);
  char hour[16];
  if (e.start <= now) {
    clockText(end, hour, sizeof(hour));
    if (e.all_day) strlcpy(out, "aujourd'hui", cap);
    else snprintf(out, cap, brief ? "jusqu'à %s" : "en ce moment, jusqu'à %s", hour);
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
  else if (brief) snprintf(day, sizeof(day), "%.3s. %d %s", DAY_NAMES[start.tm_wday], start.tm_mday, MONTH_SHORT[start.tm_mon]);
  else snprintf(day, sizeof(day), "%s %d%s %s", DAY_NAMES[start.tm_wday], start.tm_mday, start.tm_mday == 1 ? "er" : "",
                MONTH_NAMES[start.tm_mon]);
  clockText(start, hour, sizeof(hour));
  if (e.all_day) strlcpy(out, day, cap);
  else snprintf(out, cap, "%s · %s", day, hour);
}

static void draw(AgendaList which) {
  static Agenda a;  // hors de la pile : ~1 Ko
  agendaGet(which, a);
  time_t now = time(nullptr);
  // Le hackerspace : cinq événements, sur toute la largeur. Les agendas personnels : jusqu'à dix,
  // sur deux colonnes dès qu'il y en a plus de cinq.
  bool lghs = which == AGENDA_LGHS;
  uint8_t count = min<uint8_t>(a.count, lghs ? ROWS : 2 * ROWS), rows = min<uint8_t>(count, ROWS);
  int16_t col_w = count > ROWS ? (CONTENT_W - COL_GAP) / 2 : CONTENT_W;
  int16_t y = PAGE_Y + (PAGE_HEIGHT - HEAD_H - rows * ROW_H) / 2;
  // En tête : le logo et le nom du hackerspace, ou un simple titre pour les agendas personnels
  int16_t title_x = MARGIN_X + (lghs ? LOGO_W + 20 : 0);

  gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);
  if (lghs) gfxText(MARGIN_X, y, "a", font_lghs56, COLOR_TEXT, COLOR_BG);
  gfxTextBox(title_x, y + (font_lghs56.line_height - font_sans40.line_height) / 2, LCD_WIDTH - MARGIN_X - title_x,
             lghs ? "Liège Hackerspace" : "Agenda", font_sans40, COLOR_TEXT, COLOR_BG);
  y += HEAD_H;
  for (uint8_t i = 0; i < count; i++) {
    int16_t x = MARGIN_X + (i / ROWS) * (col_w + COL_GAP), row_y = y + (i % ROWS) * ROW_H;
    char when[112], name[AGENDA_NAME_SIZE] = "";
    whenText(a.events[i], now, count > ROWS, when, sizeof(when));
    // Agenda personnel : son nom, s'il en a un, à la suite de la date
    if (!lghs) agendaName(a.source[i], name, sizeof(name));
    if (name[0]) {
      toLatin1(name);
      strlcat(when, " · ", sizeof(when));
      strlcat(when, name, sizeof(when));
    }
    gfxTextBox(x, row_y, col_w, when, font_sans24, COLOR_DIM, COLOR_BG);
    gfxTextBox(x, row_y + 30, col_w, a.events[i].title, font_sans32, COLOR_TEXT, COLOR_BG);
  }
}

static void update(AgendaList which) {
  uint32_t version = agendaVersion(which);
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  if (version == 0 || (version == shown[which] && t.tm_yday == shown_day[which])) return;  // à 0, la page va céder la place
  shown[which] = version;
  shown_day[which] = t.tm_yday;
  draw(which);
}

// Une page par liste : les mêmes fonctions, chacune pour la sienne
static bool lghsActive() { return agendaVersion(AGENDA_LGHS) != 0; }
static void lghsUpdate() { update(AGENDA_LGHS); }
static void lghsShow() { shown[AGENDA_LGHS] = 0, update(AGENDA_LGHS); }
static bool mineActive() { return agendaVersion(AGENDA_MINE) != 0; }
static void mineUpdate() { update(AGENDA_MINE); }
static void mineShow() { shown[AGENDA_MINE] = 0, update(AGENDA_MINE); }

extern const Plugin agenda_plugin = {"agenda", false, agendaBegin, lghsActive, lghsShow, lghsUpdate, nullptr,
                                     false,    false, "Agenda LGHS"};
extern const Plugin my_agenda_plugin = {"perso", false, agendaBegin, mineActive, mineShow, mineUpdate, nullptr,
                                        false,   false, "Mes agendas"};
