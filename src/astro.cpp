#include "astro.h"
#include <Preferences.h>

#define J2000_DAYS  10957.5  // le 1er janvier 2000 à midi UTC, en jours depuis 1970

static const double RAD = M_PI / 180, TILT = 23.4397 * RAD;  // inclinaison de l'axe de la Terre

// Deux octets sans lien entre eux, écrits par le back office et lus par la boucle d'affichage
static AstroSettings settings = {true, true};

// Anomalie moyenne du soleil et sa longitude écliptique, en radians, `days` jours après J2000
static void sunPosition(double days, double &anomaly, double &lon) {
  anomaly = (357.5291 + 0.98560028 * days) * RAD;
  lon = anomaly + (1.9148 * sin(anomaly) + 0.02 * sin(2 * anomaly) + 282.9372) * RAD;
}

float sunElevation(time_t t, float latitude, float longitude) {
  double days = t / 86400.0 - J2000_DAYS, anomaly, lon;
  sunPosition(days, anomaly, lon);
  double declination = asin(sin(lon) * sin(TILT));
  double ascension = atan2(sin(lon) * cos(TILT), cos(lon));
  double hour_angle = (280.147 + 360.9856235 * days + longitude) * RAD - ascension;
  double lat = latitude * RAD;
  return asin(sin(lat) * sin(declination) + cos(lat) * cos(declination) * cos(hour_angle)) / RAD;
}

SunDay sunTimes(time_t noon, float latitude, float longitude, time_t &rise, time_t &set) {
  // Midi solaire moyen à cette longitude, le plus proche de `noon`
  double days = round(noon / 86400.0 - J2000_DAYS + longitude / 360) - longitude / 360, anomaly, lon;
  sunPosition(days, anomaly, lon);
  double transit = days + 0.0053 * sin(anomaly) - 0.0069 * sin(2 * lon);  // midi solaire vrai
  double declination = asin(sin(lon) * sin(TILT)), lat = latitude * RAD;

  // Angle horaire du soleil à l'horizon. -0,833° : son bord supérieur, relevé par la réfraction.
  double c = (sin(-0.833 * RAD) - sin(lat) * sin(declination)) / (cos(lat) * cos(declination));
  if (c < -1) return SUN_ALWAYS_UP;
  if (c > 1) return SUN_ALWAYS_DOWN;
  double half_day = acos(c) / (2 * M_PI);  // du lever à midi, en jours
  rise = (time_t)llround((transit - half_day + J2000_DAYS) * 86400);
  set = (time_t)llround((transit + half_day + J2000_DAYS) * 86400);
  return SUN_RISES;
}

uint8_t moonPhase(time_t t) {
  double days = t / 86400.0 - J2000_DAYS, sun_anomaly, sun_lon;
  sunPosition(days, sun_anomaly, sun_lon);
  // Longitude de la lune : sa longitude moyenne et ses cinq plus grandes inégalités
  double anomaly = (134.9634 + 13.06499295 * days) * RAD;
  double elongation = (297.8502 + 12.19074912 * days) * RAD;  // écart moyen au soleil
  double lon = 218.3165 + 13.17639648 * days + 6.289 * sin(anomaly) + 1.274 * sin(2 * elongation - anomaly) +
               0.658 * sin(2 * elongation) + 0.214 * sin(2 * anomaly) - 0.186 * sin(sun_anomaly);
  // Ecart réel au soleil : 0° nouvelle lune, 180° pleine lune. Chaque phase couvre 45°, centrée sur son nom.
  double apart = fmod(lon - sun_lon / RAD, 360);
  if (apart < 0) apart += 360;
  return (uint8_t)((apart + 22.5) / 45) % 8;
}

void astroBegin() {
  Preferences prefs;
  prefs.begin("ecran");
  settings.sun = prefs.getBool("soleil", settings.sun);
  settings.moon = prefs.getBool("lune", settings.moon);
  prefs.end();
}

void astroSettings(AstroSettings &out) {
  out = settings;
}

void astroConfigure(const AstroSettings &s) {
  if (s.sun == settings.sun && s.moon == settings.moon) return;
  settings = s;
  Preferences prefs;
  prefs.begin("ecran");
  prefs.putBool("soleil", s.sun);
  prefs.putBool("lune", s.moon);
  prefs.end();
}
