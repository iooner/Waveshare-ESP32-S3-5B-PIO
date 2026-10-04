#include "brightness.h"
#include <Preferences.h>
#include "board.h"
#include "net.h"
#include "weather.h"

#define TWILIGHT_DEG  6.0f   // fondu entre 6° sous l'horizon (fin du crépuscule civil) et 6° au-dessus
#define UPDATE_MS     10000  // le soleil monte d'un degré en 6 minutes au plus vite

// Lus par la boucle d'affichage, écrits par le back office depuis une autre tâche. Trois octets
// sans lien entre eux : au pire, un mélange d'ancien et de nouveau pendant une image.
static BrightnessSettings settings = {10, 2, false};
static volatile bool refresh = true;  // réglages changés : à appliquer sans attendre
static int16_t applied = -1;          // luminosité en place

// Hauteur du soleil au-dessus de l'horizon, en degrés. Formules de position du soleil à
// l'ordre le plus bas (anomalie moyenne, équation du centre, temps sidéral) : justes à une
// fraction de degré, bien assez pour un fondu.
static float sunElevation(time_t t, float latitude, float longitude) {
  const double RAD = M_PI / 180, TILT = 23.4397 * RAD;  // inclinaison de l'axe de la Terre
  double days = t / 86400.0 - 10957.5;                  // depuis le 1er janvier 2000 à midi UTC
  double anomaly = (357.5291 + 0.98560028 * days) * RAD;
  double lon = anomaly + (1.9148 * sin(anomaly) + 0.02 * sin(2 * anomaly) + 282.9372) * RAD;  // longitude écliptique
  double declination = asin(sin(lon) * sin(TILT));
  double ascension = atan2(sin(lon) * cos(TILT), cos(lon));
  double hour_angle = (280.147 + 360.9856235 * days + longitude) * RAD - ascension;
  double lat = latitude * RAD;
  return asin(sin(lat) * sin(declination) + cos(lat) * cos(declination) * cos(hour_angle)) / RAD;
}

static uint8_t wanted() {
  BrightnessSettings s = settings;
  if (!s.automatic || !netTimeSynced()) return s.day;
  WeatherSettings place;
  weatherSettings(place);
  float sun = sunElevation(time(nullptr), place.latitude, place.longitude);
  float k = constrain((sun + TWILIGHT_DEG) / (2 * TWILIGHT_DEG), 0.0f, 1.0f);
  return lroundf(s.night + (s.day - s.night) * k);
}

void brightnessBegin() {
  Preferences prefs;
  prefs.begin("ecran");
  settings.day = prefs.getUChar("jour", settings.day);
  settings.night = prefs.getUChar("nuit", settings.night);
  settings.automatic = prefs.getBool("auto", settings.automatic);
  prefs.end();
  brightnessLoop();
}

void brightnessLoop() {
  static uint32_t last = 0;
  if (!refresh && millis() - last < UPDATE_MS) return;
  refresh = false;
  last = millis();
  uint8_t level = wanted();
  if (level == applied) return;
  backlightSet(level);
  applied = level;
  Serial.printf("Luminosité : %u %%\n", level);
}

uint8_t brightnessCurrent() {
  return max<int16_t>(applied, 0);
}

void brightnessSettings(BrightnessSettings &out) {
  out = settings;
}

void brightnessConfigure(const BrightnessSettings &s) {
  BrightnessSettings n = {constrain(s.day, (uint8_t)1, (uint8_t)100), min<uint8_t>(s.night, 100), s.automatic};
  if (n.day == settings.day && n.night == settings.night && n.automatic == settings.automatic) return;
  settings = n;
  refresh = true;

  Preferences prefs;
  prefs.begin("ecran");
  prefs.putUChar("jour", n.day);
  prefs.putUChar("nuit", n.night);
  prefs.putBool("auto", n.automatic);
  prefs.end();
}
