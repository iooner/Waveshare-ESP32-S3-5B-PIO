// Horloge, sous deux formes : la barre du haut (date à gauche, heure à droite) et la page
// plein écran (heure et date en grand, météo en dessous). L'heure vient du réseau (net.h), la
// météo de weather.h, le soleil et la lune d'astro.h.
#include <time.h>
#include "astro.h"
#include "fonts/font_clock.h"
#include "fonts/font_sans24.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans40.h"
#include "fonts/font_sans48.h"
#include "fonts/font_weather64.h"
#include "net.h"
#include "plugin.h"
#include "weather.h"

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
#define DIGITS_TOP  68   // les chiffres de l'heure commencent 68 pixels sous le haut de leur case...
#define DIGITS_H    149  // ...et en font 149 de haut

// Les chiffres de l'horloge, dans une case arrêtée à leur pied : celle de la police descend
// 55 pixels plus bas et effacerait la ligne du soleil et de la lune à chaque seconde
static const GfxFont clock_digits = {font_clock.bitmap, font_clock.glyphs, font_clock.first, font_clock.last, 218};

// Météo : une colonne pour maintenant, puis une par heure à venir. Les pictogrammes tombent pile
// sur les cases du pilote d'écran, et chaque rangée tient dans ses 16 couleurs (lcd.h) : une
// seule couleur de texte, ou les trois des pictogrammes, qui n'ont que 5 niveaux d'antialiasing.
#define WX_COLS     7
#define WX_COL_W    128
#define WX_X        ((LCD_WIDTH - WX_COLS * WX_COL_W) / 2)
#define COLOR_SUN   RGB565(255, 185, 30)
#define COLOR_RAIN  RGB565(110, 170, 255)
#define ICON_LAYER  10  // la seconde couche d'un pictogramme est 10 caractères plus loin dans la police

static uint32_t shown_weather;  // version de la météo affichée, 0 = rien
static time_t shown_hour;       // première heure de prévision affichée
static char shown_alert[40];    // annonce de pluie affichée
static char shown_sky[96];      // ligne du soleil et de la lune affichée

// Disposition de la page : l'heure, puis le soleil et la lune en petit, la date, et la météo
// (annonce de pluie sur toute la largeur, puis les rangées de la bande). Le soleil et la lune,
// comme la météo, s'activent dans le back office : les blocs présents sont empilés et
// l'ensemble est centré en hauteur, pour que l'écran reste équilibré.
static struct {
  bool sky_on, weather_on;
  int16_t time, sky, date;                 // haut de chaque ligne ; date = 0 : pas encore calculé
  int16_t alert, label, icon, temp, rain;  // météo
} at;

// Recalcule la disposition. Vrai si elle a changé : tout est alors à redessiner ailleurs.
static bool arrange() {
  AstroSettings astro;
  astroSettings(astro);
  WeatherSettings weather;
  weatherSettings(weather);
  bool sky_on = astro.sun || astro.moon;
  if (at.date && sky_on == at.sky_on && weather.enabled == at.weather_on) return false;
  at.sky_on = sky_on;
  at.weather_on = weather.enabled;

  // Hauteurs comptées depuis le haut des chiffres
  int16_t sky = DIGITS_H + 17;
  int16_t date = sky_on ? sky + font_sans24.line_height + 6 : DIGITS_H + 26;
  int16_t total = date + (at.weather_on ? 337 : font_sans48.line_height);
  int16_t top = (LCD_HEIGHT - total) / 2;
  at.time = top - DIGITS_TOP;
  at.sky = top + sky;
  at.date = top + date;
  at.alert = at.date + 70;
  at.label = at.date + 118;
  at.icon = at.date + 168;
  at.temp = at.date + 236;
  at.rain = at.date + 292;
  return true;
}

// Plus rien de ce qui est noté comme affiché ne l'est : tout sera redessiné, textes vides compris
static void forgetShown() {
  memset(shown_time, 0, sizeof(shown_time));
  shown_date[0] = shown_sky[0] = shown_alert[0] = 1;
  shown_weather = UINT32_MAX;
}

// Caractère de font_weather64 pour un code météo WMO
static char weatherIcon(uint8_t code, bool day) {
  if (code == 0) return day ? 'a' : 'b';                     // ciel dégagé
  if (code <= 2) return day ? 'c' : 'd';                     // peu nuageux
  if (code == 3) return 'e';                                 // couvert
  if (code <= 48) return 'f';                                // brouillard
  if (code <= 57) return 'g';                                // bruine
  if (code <= 67 || (code >= 80 && code <= 82)) return 'h';  // pluie, averses
  if (code <= 86) return 'i';                                // neige
  return 'j';                                                // orage
}

// Un pictogramme a deux couches : la première en blanc, sauf le soleil seul ; la seconde est
// l'astre derrière le nuage, la pluie ou l'éclair
static void drawWeatherIcon(int16_t x, char icon) {
  char text[2] = {icon, 0};
  gfxTextBox(x, at.icon, WX_COL_W, text, font_weather64, icon == 'a' ? COLOR_SUN : COLOR_TEXT, COLOR_BG, GFX_CENTER);
  if (!icon) return;
  text[0] += ICON_LAYER;
  uint16_t over = icon == 'g' || icon == 'h' ? COLOR_RAIN : icon == 'd' ? COLOR_TEXT : COLOR_SUN;
  gfxText(x + (WX_COL_W - font_weather64.line_height) / 2, at.icon, text, font_weather64, over);
}

// p nul : colonne vide
static void drawWeatherColumn(uint8_t col, const char *label, const WeatherPoint *p) {
  int16_t x = WX_X + col * WX_COL_W;
  char temp[8] = "", rain[8] = "";
  if (p) {
    snprintf(temp, sizeof(temp), "%d°", p->temp);
    if (p->rain) snprintf(rain, sizeof(rain), "%d %%", p->rain);
  }
  gfxTextBox(x, at.label, WX_COL_W, label, font_sans32, COLOR_DIM, COLOR_BG, GFX_CENTER);
  drawWeatherIcon(x, p ? weatherIcon(p->code, p->day) : 0);
  gfxTextBox(x, at.temp, WX_COL_W, temp, font_sans40, COLOR_TEXT, COLOR_BG, GFX_CENTER);
  gfxTextBox(x, at.rain, WX_COL_W, rain, font_sans32, COLOR_RAIN, COLOR_BG, GFX_CENTER);
}

// Annonce de pluie d'après les prévisions au quart d'heure : quand elle arrive, ou quand elle
// s'arrête s'il pleut déjà. Vide s'il n'y a rien à annoncer dans les heures qui viennent.
static void rainAlert(const Weather &w, time_t now, char *out, size_t cap) {
  out[0] = 0;
  int32_t first = (now - w.quarters_from) / 900;  // quart d'heure en cours
  if (first < 0 || first >= w.quarter_count) return;
  auto wet = [&](int32_t i) { return (w.rain_quarters >> i & 1) != 0; };
  int32_t i = first;
  while (i < w.quarter_count && wet(i) == wet(first)) i++;  // premier quart d'heure où le temps change
  time_t change = w.quarters_from + i * 900;

  if (!wet(first)) {
    if (i == w.quarter_count) return;
    int32_t mins = max<int32_t>(5, (change - now + 150) / 300 * 5);  // à 5 minutes près
    if (mins < 60) snprintf(out, cap, "Pluie dans %ld min", (long)mins);
    else snprintf(out, cap, "Pluie dans %ld h %02ld", (long)(mins / 60), (long)(mins % 60));
  } else if (i == w.quarter_count) {
    strlcpy(out, "Pluie pour plusieurs heures", cap);
  } else {
    struct tm t;
    localtime_r(&change, &t);
    snprintf(out, cap, "Pluie jusqu'à %d h %02d", t.tm_hour, t.tm_min);
  }
}

static const char *const MOON[] = {"Nouvelle lune",           "Lune : premier croissant", "Lune : premier quartier",
                                   "Lune gibbeuse croissante", "Pleine lune",              "Lune gibbeuse décroissante",
                                   "Lune : dernier quartier",  "Lune : dernier croissant"};

// Lever et coucher du soleil du jour et phase de la lune, selon ce qui est activé dans le back
// office. Vide si rien ne l'est, ou tant que l'heure n'est pas reçue.
static void almanac(time_t now, char *out, size_t cap) {
  out[0] = 0;
  if (!netTimeSynced()) return;
  AstroSettings shown;
  astroSettings(shown);
  if (shown.sun) {
    WeatherSettings place;
    weatherSettings(place);
    struct tm noon;
    localtime_r(&now, &noon);
    noon.tm_hour = 12;
    noon.tm_min = noon.tm_sec = 0;
    noon.tm_isdst = -1;
    time_t rise, set;
    SunDay day = sunTimes(mktime(&noon), place.latitude, place.longitude, rise, set);
    if (day == SUN_RISES) {
      struct tm r, s;
      rise += 30, set += 30;  // à la minute la plus proche
      localtime_r(&rise, &r);
      localtime_r(&set, &s);
      snprintf(out, cap, "Lever %d h %02d · Coucher %d h %02d", r.tm_hour, r.tm_min, s.tm_hour, s.tm_min);
    } else {
      strlcpy(out, day == SUN_ALWAYS_UP ? "Soleil de minuit" : "Nuit polaire", cap);
    }
  }
  if (shown.moon) {
    if (out[0]) strlcat(out, " · ", cap);
    strlcat(out, MOON[moonPhase(now)], cap);
  }
}

static void skyUpdate() {
  if (!at.sky_on) return;
  static char sky[sizeof(shown_sky)];  // refait une fois par seconde : le calcul ne vaut pas chaque image
  static time_t sky_at = -1;
  time_t now = time(nullptr);
  if (now != sky_at) almanac(now, sky, sizeof(sky));
  sky_at = now;
  if (strcmp(sky, shown_sky) == 0) return;
  gfxTextBox(MARGIN_X, at.sky, CONTENT_W, sky, font_sans24, COLOR_DIM, COLOR_BG, GFX_CENTER);
  strlcpy(shown_sky, sky, sizeof(shown_sky));
}

static void weatherUpdate() {
  if (!at.weather_on) return;
  Weather w;
  uint32_t version = weatherGet(w);
  if (!version) w.hour_count = w.quarter_count = 0;
  time_t secs = time(nullptr);

  char alert[sizeof(shown_alert)];
  rainAlert(w, secs, alert, sizeof(alert));
  if (strcmp(alert, shown_alert) != 0) {
    gfxTextBox(MARGIN_X, at.alert, CONTENT_W, alert, font_sans32, COLOR_RAIN, COLOR_BG, GFX_CENTER);
    strlcpy(shown_alert, alert, sizeof(shown_alert));
  }

  // Les prévisions commencent à la prochaine heure : la bande avance d'une colonne quand
  // l'heure change, sans attendre la lecture suivante
  uint8_t first = 0;
  while (first < w.hour_count && w.hours[first].time <= secs) first++;
  time_t hour = first < w.hour_count ? w.hours[first].time : 0;
  if (version == shown_weather && hour == shown_hour) return;
  shown_weather = version;
  shown_hour = hour;

  if (first > 0) w.now.rain = w.hours[first - 1].rain;  // risque de pluie de l'heure en cours
  drawWeatherColumn(0, version ? "Maint." : "", version ? &w.now : nullptr);
  for (uint8_t col = 1; col < WX_COLS; col++) {
    uint8_t i = first + col - 1;
    const WeatherPoint *p = i < w.hour_count ? &w.hours[i] : nullptr;
    char label[8] = "";
    if (p) {
      struct tm t;
      localtime_r(&p->time, &t);
      snprintf(label, sizeof(label), "%d h", t.tm_hour);
    }
    drawWeatherColumn(col, label, p);
  }
}

// "HH:MM:SS" centré. Les chiffres ont tous la même largeur : chaque caractère garde sa place,
// on ne redessine que ceux qui changent (un seul chiffre la plupart des secondes).
static void drawBigTime(const char *text) {
  int16_t x = (LCD_WIDTH - gfxTextWidth(text, clock_digits)) / 2;
  for (uint8_t i = 0; text[i]; i++) {
    char one[2] = {text[i], 0};
    if (text[i] != shown_time[i]) gfxText(x, at.time, one, clock_digits, COLOR_TEXT, COLOR_BG);
    x += gfxTextWidth(one, clock_digits);
    shown_time[i] = text[i];
  }
}

static void bigUpdate() {
  if (arrange()) {
    // Un bloc est apparu ou a disparu : tout change de place. L'écran est effacé et présenté avant
    // d'être redessiné, comme à l'arrivée d'une page.
    gfxClear(COLOR_BG);
    lcdPresent();
    forgetShown();
  }
  struct tm t;
  char time_text[9], date_text[48];
  if (now(t, date_text, sizeof(date_text))) {
    snprintf(time_text, sizeof(time_text), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
    drawBigTime(time_text);
  }
  if (strcmp(date_text, shown_date) != 0) {
    gfxTextBox(0, at.date, LCD_WIDTH, date_text, font_sans48, COLOR_TEXT, COLOR_BG, GFX_CENTER);
    strlcpy(shown_date, date_text, sizeof(shown_date));
  }
  skyUpdate();
  weatherUpdate();
}

static void bigShow() {
  forgetShown();
  bigUpdate();
}

extern const Plugin clock_bar_plugin = {"barre", false, nullptr, nullptr, barShow, barUpdate, nullptr};
extern const Plugin clock_plugin = {"horloge", true, weatherBegin, nullptr, bigShow, bigUpdate, nullptr};
