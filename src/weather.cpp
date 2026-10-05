#include "weather.h"
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/idf_additions.h>
#include "json.h"
#include "net.h"
#include "secrets.h"

// Lieu des prévisions tant qu'aucun n'a été choisi dans le back office. A défaut : Bruxelles.
#ifndef WEATHER_LATITUDE
#define WEATHER_LATITUDE   50.85
#define WEATHER_LONGITUDE  4.35
#endif

#define HOST             "api.open-meteo.com"
#define HTTP_TIMEOUT_MS  5000
#define REFRESH_MS       (15UL * 60 * 1000)  // Open-Meteo recalcule le temps actuel tous les quarts d'heure
#define RETRY_MS         (60UL * 1000)       // après un échec
#define MAX_AGE_MS       (90UL * 60 * 1000)  // au-delà, mieux vaut ne rien afficher qu'une météo périmée
#define RESP_SIZE        2048                // la réponse fait ~1,3 Ko
#define RAIN_MM          0.1                 // précipitations en un quart d'heure à partir desquelles il pleut

static Weather current;
static WeatherSettings settings = {true, WEATHER_LATITUDE, WEATHER_LONGITUDE};
static uint32_t version = 0, fetched_at;
static bool valid = false;  // `current` est la météo du lieu réglé
static SemaphoreHandle_t lock;
static TaskHandle_t task;
static char *resp;  // réponse HTTP en cours, en PSRAM

// --- Lecture du JSON ---
// La réponse est un objet à plat de nombres et de tableaux de nombres : pas besoin d'un vrai
// analyseur, on cherche chaque clé à partir du début de son objet.

// Lit le tableau de nombres `key`, `cap` valeurs au plus. Renvoie le nombre de valeurs lues.
static uint8_t numbers(const char *from, const char *key, double *out, uint8_t cap) {
  const char *p = jsonValue(from, key);
  if (!p || *p != '[') return 0;
  uint8_t n = 0;
  while (n < cap) {
    char *end;
    out[n] = strtod(++p, &end);  // saute le crochet ou la virgule
    if (end == p) {
      if (strncmp(p, "null", 4) != 0) break;
      end += 4;  // valeur absente : 0
    }
    n++;
    p = end;
    if (*p != ',') break;
  }
  return n;
}

// --- Réseau ---

// Lit la météo et la publie. Faux si le serveur ne répond pas ou si la réponse est incomplète.
static bool fetch() {
  WeatherSettings place;
  weatherSettings(place);
  WiFiClient c;
  if (!c.connect(HOST, 80, HTTP_TIMEOUT_MS)) return false;
  // HTTP/1.0 : la réponse arrive d'un bloc, puis le serveur ferme la connexion
  c.printf("GET /v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,is_day"
           "&hourly=temperature_2m,weather_code,precipitation_probability,is_day&forecast_hours=%d"
           "&minutely_15=precipitation&forecast_minutely_15=%d&timeformat=unixtime&timezone=auto HTTP/1.0\r\nHost: " HOST "\r\n\r\n",
           place.latitude, place.longitude, WEATHER_HOURS, WEATHER_QUARTERS);
  size_t len = 0;
  uint32_t t0 = millis();
  while (len < RESP_SIZE - 1 && millis() - t0 < HTTP_TIMEOUT_MS && (c.connected() || c.available())) {
    int got = c.read((uint8_t *)resp + len, RESP_SIZE - 1 - len);
    if (got > 0) len += got;
    else delay(2);
  }
  resp[len] = 0;
  c.stop();
  if (!strstr(resp, " 200 ")) return false;

  // Les objets de la réponse ont des clés en commun : chacune est cherchée à partir de son objet
  const char *now = jsonValue(resp, "current"), *hourly = jsonValue(now, "hourly");
  const char *temp = jsonValue(now, "temperature_2m"), *code = jsonValue(now, "weather_code"), *day = jsonValue(now, "is_day");
  if (!hourly || !temp || !code || !day) return false;
  Weather w = {};
  w.now = {0, (int8_t)lround(atof(temp)), (uint8_t)atoi(code), 0, atoi(day) != 0};

  static const char *const KEYS[] = {"time", "temperature_2m", "weather_code", "precipitation_probability", "is_day"};
  static_assert(WEATHER_QUARTERS <= WEATHER_HOURS, "v sert aussi aux quarts d'heure");
  double v[5][WEATHER_HOURS];
  w.hour_count = WEATHER_HOURS;
  for (uint8_t k = 0; k < 5; k++) w.hour_count = min(w.hour_count, numbers(hourly, KEYS[k], v[k], WEATHER_HOURS));
  if (w.hour_count == 0) return false;
  for (uint8_t i = 0; i < w.hour_count; i++) {
    w.hours[i] = {(time_t)v[0][i], (int8_t)lround(v[1][i]), (uint8_t)v[2][i], (uint8_t)v[3][i], v[4][i] != 0};
  }

  // Pluie par quart d'heure. Si elle manque, le reste de la météo est gardé.
  const char *quarters = jsonValue(resp, "minutely_15");
  w.quarter_count = min(numbers(quarters, "time", v[0], WEATHER_QUARTERS),
                        numbers(quarters, "precipitation", v[1], WEATHER_QUARTERS));
  w.quarters_from = w.quarter_count ? (time_t)v[0][0] : 0;
  for (uint8_t i = 0; i < w.quarter_count; i++) {
    if (v[1][i] >= RAIN_MM) w.rain_quarters |= 1 << i;
  }

  // L'heure locale suit le fuseau du lieu. Un décalage fixe ne change d'heure, été comme hiver,
  // qu'à la lecture suivante : dans le fuseau par défaut, on garde donc sa règle complète.
  const char *zone = jsonValue(resp, "timezone"), *offset = jsonValue(resp, "utc_offset_seconds");
  if (zone && offset) {
    bool home = strncmp(zone, "\"" NET_TIMEZONE_NAME "\"", sizeof(NET_TIMEZONE_NAME) + 1) == 0;
    netSetTimezone(home ? NET_TIMEZONE_DEFAULT : atoi(offset));
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  current = w;
  valid = true;
  version++;
  fetched_at = millis();
  xSemaphoreGive(lock);
  Serial.printf("Météo : %d °C, code %u, %u heures de prévision\n", w.now.temp, w.now.code, w.hour_count);
  return true;
}

static void weatherTask(void *) {
  for (;;) {
    uint32_t wait = 500;  // en attendant le Wi-Fi
    if (WiFi.status() == WL_CONNECTED) {
      bool ok = fetch();
      if (!ok) Serial.println("Météo : pas de réponse");
      wait = ok ? REFRESH_MS : RETRY_MS;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));  // un changement de réglages réveille la tâche
  }
}

void weatherBegin() {
  lock = xSemaphoreCreateMutex();
  Preferences prefs;
  prefs.begin("meteo");
  settings.enabled = prefs.getBool("on", settings.enabled);
  settings.latitude = prefs.getFloat("lat", settings.latitude);
  settings.longitude = prefs.getFloat("lon", settings.longitude);
  prefs.end();
  resp = (char *)heap_caps_malloc(RESP_SIZE, MALLOC_CAP_SPIRAM);
  // Pile en PSRAM, pour garder la RAM interne à l'écran : permis parce que la tâche n'écrit jamais en
  // flash (les réglages sont enregistrés par la tâche qui appelle weatherConfigure())
  if (resp) xTaskCreatePinnedToCoreWithCaps(weatherTask, "meteo", 5120, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);
}

uint32_t weatherGet(Weather &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = current;
  uint32_t v = settings.enabled && valid && millis() - fetched_at < MAX_AGE_MS ? version : 0;
  xSemaphoreGive(lock);
  return v;
}

int32_t weatherAge() {
  xSemaphoreTake(lock, portMAX_DELAY);
  int32_t age = version ? (millis() - fetched_at) / 1000 : -1;
  xSemaphoreGive(lock);
  return age;
}

void weatherSettings(WeatherSettings &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = settings;
  xSemaphoreGive(lock);
}

void weatherConfigure(const WeatherSettings &s) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool moved = s.latitude != settings.latitude || s.longitude != settings.longitude;
  bool changed = moved || s.enabled != settings.enabled;
  settings = s;
  if (moved) valid = false;  // la météo de l'ancien lieu ne doit plus s'afficher
  xSemaphoreGive(lock);
  if (!changed) return;

  Preferences prefs;
  prefs.begin("meteo");
  prefs.putBool("on", s.enabled);
  prefs.putFloat("lat", s.latitude);
  prefs.putFloat("lon", s.longitude);
  prefs.end();
  Serial.printf("Météo : %s, lieu %.4f, %.4f\n", s.enabled ? "activée" : "désactivée", s.latitude, s.longitude);
  if (task) xTaskNotifyGive(task);
}
