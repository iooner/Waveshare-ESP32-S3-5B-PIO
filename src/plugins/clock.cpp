// Horloge, sous deux formes : la barre du haut (date à gauche, heure à droite) et la page
// plein écran (heure et date en grand). L'heure vient du réseau (net.h).
#include <time.h>
#include "fonts/font_clock.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans48.h"
#include "net.h"
#include "plugin.h"

static const char *const DAYS[] = {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"};
static const char *const MONTHS[] = {"janvier", "février", "mars",      "avril",   "mai",      "juin",
                                     "juillet", "août",    "septembre", "octobre", "novembre", "décembre"};

// Heure locale, ou faux tant qu'elle n'a pas été reçue : `date` contient alors l'état du réseau
static bool now(struct tm &t, char *date, size_t cap) {
  if (!netTimeSynced()) {
    strlcpy(date, netConnected() ? "Synchronisation de l'heure..." : "Connexion au Wi-Fi...", cap);
    return false;
  }
  time_t secs = time(nullptr);
  localtime_r(&secs, &t);
  snprintf(date, cap, "%s %d%s %s %d", DAYS[t.tm_wday], t.tm_mday, t.tm_mday == 1 ? "er" : "", MONTHS[t.tm_mon],
           t.tm_year + 1900);
  return true;
}

// Ce qui est actuellement à l'écran, pour ne redessiner que ce qui change
static char shown_time[9], shown_date[48];

// --- Barre ---
#define BAR_TIME_W  200  // largeur réservée à "HH:MM"
#define BAR_TIME_Y  ((BAR_HEIGHT - 67) / 2)
#define BAR_DATE_Y  ((BAR_HEIGHT - 45) / 2 + 4)

static void barUpdate() {
  struct tm t;
  char time_text[9] = "", date_text[48];
  if (now(t, date_text, sizeof(date_text))) snprintf(time_text, sizeof(time_text), "%02d:%02d", t.tm_hour, t.tm_min);

  if (strcmp(time_text, shown_time) != 0) {
    gfxTextBox(LCD_WIDTH - MARGIN_X - BAR_TIME_W, BAR_TIME_Y, BAR_TIME_W, time_text, font_sans48, COLOR_TEXT, COLOR_BG,
               GFX_RIGHT);
    strlcpy(shown_time, time_text, sizeof(shown_time));
  }
  if (strcmp(date_text, shown_date) != 0) {
    gfxTextBox(MARGIN_X, BAR_DATE_Y, CONTENT_W - BAR_TIME_W, date_text, font_sans32, COLOR_TEXT, COLOR_BG);
    strlcpy(shown_date, date_text, sizeof(shown_date));
  }
}

static void barShow() {
  shown_time[0] = shown_date[0] = 1;  // différent de tout texte : force le dessin
  barUpdate();
}

// --- Page plein écran ---
#define BIG_TIME_Y  110
#define BIG_DATE_Y  400

// "HH:MM:SS" centré. Les chiffres ont tous la même largeur : chaque caractère garde sa place,
// on ne redessine que ceux qui changent (un seul chiffre la plupart des secondes).
static void drawBigTime(const char *text) {
  int16_t x = (LCD_WIDTH - gfxTextWidth(text, font_clock)) / 2;
  for (uint8_t i = 0; text[i]; i++) {
    char one[2] = {text[i], 0};
    if (text[i] != shown_time[i]) gfxText(x, BIG_TIME_Y, one, font_clock, COLOR_TEXT, COLOR_BG);
    x += gfxTextWidth(one, font_clock);
    shown_time[i] = text[i];
  }
}

static void bigUpdate() {
  struct tm t;
  char time_text[9], date_text[48];
  if (now(t, date_text, sizeof(date_text))) {
    snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    drawBigTime(time_text);
  }
  if (strcmp(date_text, shown_date) != 0) {
    gfxTextBox(0, BIG_DATE_Y, LCD_WIDTH, date_text, font_sans48, COLOR_TEXT, COLOR_BG, GFX_CENTER);
    strlcpy(shown_date, date_text, sizeof(shown_date));
  }
}

static void bigShow() {
  memset(shown_time, 0, sizeof(shown_time));
  shown_date[0] = 1;
  bigUpdate();
}

extern const Plugin clock_bar_plugin = {"barre", false, nullptr, nullptr, barShow, barUpdate, nullptr};
extern const Plugin clock_plugin = {"horloge", true, nullptr, nullptr, bigShow, bigUpdate, nullptr};
