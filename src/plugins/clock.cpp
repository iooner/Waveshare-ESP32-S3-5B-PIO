// Barre du haut : date à gauche, heure à droite. L'heure vient du réseau (net.h).
#include <time.h>
#include "fonts/font_sans24.h"
#include "fonts/font_sans32.h"
#include "net.h"
#include "plugin.h"

#define TIME_W  140  // largeur réservée à "HH:MM"
#define TIME_Y  ((BAR_HEIGHT - 45) / 2)
#define DATE_Y  ((BAR_HEIGHT - 33) / 2 + 2)

static const char *const DAYS[] = {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"};
static const char *const MONTHS[] = {"janvier", "février", "mars",      "avril",   "mai",      "juin",
                                     "juillet", "août",    "septembre", "octobre", "novembre", "décembre"};

// Ce qui est actuellement à l'écran, pour ne redessiner que ce qui change
static char shown_time[8], shown_date[48];

static void clockUpdate() {
  char time_text[8] = "", date_text[48];
  if (netTimeSynced()) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    snprintf(time_text, sizeof(time_text), "%02d:%02d", t.tm_hour, t.tm_min);
    snprintf(date_text, sizeof(date_text), "%s %d%s %s %d", DAYS[t.tm_wday], t.tm_mday, t.tm_mday == 1 ? "er" : "",
             MONTHS[t.tm_mon], t.tm_year + 1900);
  } else {
    strlcpy(date_text, netConnected() ? "Synchronisation de l'heure..." : "Connexion au Wi-Fi...", sizeof(date_text));
  }

  if (strcmp(time_text, shown_time) != 0) {
    gfxTextBox(LCD_WIDTH - MARGIN_X - TIME_W, TIME_Y, TIME_W, time_text, font_sans32, COLOR_TEXT, COLOR_BG, GFX_RIGHT);
    strlcpy(shown_time, time_text, sizeof(shown_time));
  }
  if (strcmp(date_text, shown_date) != 0) {
    gfxTextBox(MARGIN_X, DATE_Y, CONTENT_W - TIME_W, date_text, font_sans24, COLOR_TEXT, COLOR_BG);
    strlcpy(shown_date, date_text, sizeof(shown_date));
  }
}

static void clockShow() {
  gfxFillRect(0, 0, LCD_WIDTH, BAR_HEIGHT, COLOR_BG);
  shown_time[0] = shown_date[0] = 1;  // différent de tout texte : force le dessin
  clockUpdate();
}

extern const Plugin clock_plugin = {"horloge", 0, nullptr, clockShow, clockUpdate};
