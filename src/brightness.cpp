#include "brightness.h"
#include <Preferences.h>
#include "astro.h"
#include "board.h"
#include "gfx.h"
#include "net.h"
#include "weather.h"

#define TWILIGHT_DEG  6.0f   // fondu entre 6° sous l'horizon (fin du crépuscule civil) et 6° au-dessus
#define UPDATE_MS     10000  // le soleil monte d'un degré en 6 minutes au plus vite

// Lus par la boucle d'affichage, écrits par le back office depuis une autre tâche. Des valeurs
// sans lien entre elles : au pire, un mélange d'ancien et de nouveau pendant une image.
static BrightnessSettings settings = {10, 2, false, false, false, 23 * 60, 7 * 60};
static volatile bool refresh = true;  // réglages changés : à appliquer sans attendre
static int16_t applied = -1;          // luminosité en place

// Part du jour, de 0 (nuit) à 1 (plein jour), en fondu pendant l'aube et le crépuscule.
// Plein jour tant que l'heure n'est pas reçue.
static float daylight() {
  if (!netTimeSynced()) return 1;
  WeatherSettings place;
  weatherSettings(place);
  float sun = sunElevation(time(nullptr), place.latitude, place.longitude);
  return constrain((sun + TWILIGHT_DEG) / (2 * TWILIGHT_DEG), 0.0f, 1.0f);
}

// Vrai pendant la plage d'extinction, qui peut passer minuit. Faux tant que l'heure n'est pas reçue.
static bool asleep(const BrightnessSettings &s) {
  if (!s.sleep || !netTimeSynced()) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  uint16_t minute = t.tm_hour * 60 + t.tm_min;
  if (s.sleep_from <= s.sleep_to) return minute >= s.sleep_from && minute < s.sleep_to;
  return minute >= s.sleep_from || minute < s.sleep_to;
}

void brightnessBegin() {
  Preferences prefs;
  prefs.begin("ecran");
  settings.day = prefs.getUChar("jour", settings.day);
  settings.night = prefs.getUChar("nuit", settings.night);
  settings.automatic = prefs.getBool("auto", settings.automatic);
  settings.red = prefs.getBool("rouge", settings.red);
  settings.sleep = prefs.getBool("veille", settings.sleep);
  settings.sleep_from = prefs.getUShort("debut", settings.sleep_from);
  settings.sleep_to = prefs.getUShort("fin", settings.sleep_to);
  prefs.end();
  brightnessLoop();
}

void brightnessLoop() {
  static uint32_t last = 0;
  if (!refresh && millis() - last < UPDATE_MS) return;
  refresh = false;
  last = millis();
  BrightnessSettings s = settings;
  float day = s.automatic || s.red ? daylight() : 1;
  // La page se redessine d'elle-même quand la teinte change (plugin.cpp)
  gfxSetNight(s.red ? lroundf((1 - day) * GFX_NIGHT_MAX) : 0);
  uint8_t level = s.automatic ? lroundf(s.night + (s.day - s.night) * day) : s.day;
  if (asleep(s)) level = 0;
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
  BrightnessSettings n = s;
  n.day = constrain(s.day, (uint8_t)1, (uint8_t)100);
  n.night = min<uint8_t>(s.night, 100);
  n.sleep_from = min<uint16_t>(s.sleep_from, 24 * 60 - 1);
  n.sleep_to = min<uint16_t>(s.sleep_to, 24 * 60 - 1);
  const BrightnessSettings &o = settings;
  if (n.day == o.day && n.night == o.night && n.automatic == o.automatic && n.red == o.red && n.sleep == o.sleep &&
      n.sleep_from == o.sleep_from && n.sleep_to == o.sleep_to) {
    return;
  }
  settings = n;
  refresh = true;

  Preferences prefs;
  prefs.begin("ecran");
  prefs.putUChar("jour", n.day);
  prefs.putUChar("nuit", n.night);
  prefs.putBool("auto", n.automatic);
  prefs.putBool("rouge", n.red);
  prefs.putBool("veille", n.sleep);
  prefs.putUShort("debut", n.sleep_from);
  prefs.putUShort("fin", n.sleep_to);
  prefs.end();
}
