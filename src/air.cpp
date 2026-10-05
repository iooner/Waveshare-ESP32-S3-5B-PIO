#include "air.h"
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/idf_additions.h>

#define HTTP_TIMEOUT_MS  4000
#define REFRESH_MS       (3UL * 60 * 1000)     // le rythme de mesure du capteur
#define RETRY_MS         30000                 // après un échec
#define MAX_AGE_MS       (15UL * 60 * 1000)    // au-delà, mieux vaut ne rien afficher
#define RESP_SIZE        1536                  // la réponse fait ~500 octets

static AirSettings settings = {true, "", false};
static volatile float last_pm25, last_pm10;
static volatile uint32_t read_at;
static volatile bool valid = false;
static SemaphoreHandle_t lock;  // protège les réglages
static TaskHandle_t task;
static char *resp;  // réponse HTTP en cours, en PSRAM

// Les couleurs vont du bleu au rouge sombre comme celles de l'indice, éclaircies aux deux bouts
// pour rester lisibles sur fond noir
const AirLevel AIR_LEVEL[AIR_LEVELS] = {
    {"Excellent", RGB565(90, 150, 255)},     {"Très bon", RGB565(60, 185, 255)}, {"Bon", RGB565(50, 190, 80)},
    {"Assez bon", RGB565(110, 240, 90)},     {"Moyen", RGB565(250, 240, 70)},    {"Insuffisant", RGB565(255, 190, 40)},
    {"Assez mauvais", RGB565(255, 125, 30)}, {"Mauvais", RGB565(255, 70, 60)},   {"Très mauvais", RGB565(215, 45, 60)},
    {"Exécrable", RGB565(175, 50, 90)}};
// Valeur en µg/m³ jusqu'à laquelle on reste à chaque niveau : seuils de l'indice horaire, fait pour
// juger l'air à un instant donné (https://wallonair.be/fr/en-savoir-plus/indice-de-la-qualite-de-l-air)
static const float PM25_STEPS[] = {3.5, 7.5, 10, 15, 20, 35, 50, 60, 75}, PM10_STEPS[] = {10, 20, 35, 45, 60, 80, 95, 110, 140};

static uint8_t levelOf(float value, const float *steps) {
  uint8_t level = 0;
  while (level < AIR_LEVELS - 1 && value > steps[level]) level++;
  return level;
}

uint8_t airLevelPm25(float value) {
  return levelOf(value, PM25_STEPS);
}

uint8_t airLevelPm10(float value) {
  return levelOf(value, PM10_STEPS);
}

bool airBarLevel(uint8_t &level) {
  float pm25, pm10;
  if (!settings.bar || !airGet(pm25, pm10)) return false;
  level = (airLevelPm25(pm25) + airLevelPm10(pm10) + 1) / 2;
  return true;
}

// Valeur d'une mesure dont le type finit par `suffix` : {"value_type":"SDS_P2","value":"2.60"}.
// Le préfixe dépend du modèle de capteur (SDS, PMS, SPS30...). NAN si elle manque.
static float measure(const char *json, const char *suffix) {
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "%s\",\"value\":\"", suffix);
  const char *p = strstr(json, pattern);
  return p ? atof(p + strlen(pattern)) : NAN;
}

static bool fetch(const char *host) {
  WiFiClient c;
  if (!c.connect(host, 80, HTTP_TIMEOUT_MS)) return false;
  c.printf("GET /data.json HTTP/1.0\r\nHost: %s\r\n\r\n", host);
  size_t len = 0;
  uint32_t t0 = millis();
  while (len < RESP_SIZE - 1 && millis() - t0 < HTTP_TIMEOUT_MS && (c.connected() || c.available())) {
    int got = c.read((uint8_t *)resp + len, RESP_SIZE - 1 - len);
    if (got > 0) len += got;
    else delay(2);
  }
  resp[len] = 0;
  c.stop();
  float pm10 = measure(resp, "_P1"), pm25 = measure(resp, "_P2");
  if (isnan(pm10) || isnan(pm25)) return false;
  last_pm10 = pm10;
  last_pm25 = pm25;
  read_at = millis();
  valid = true;
  return true;
}

static void airTask(void *) {
  for (;;) {
    AirSettings s;
    airSettings(s);
    uint32_t wait = REFRESH_MS;
    if (WiFi.status() != WL_CONNECTED) wait = 2000;  // en attendant le Wi-Fi
    else if (s.enabled && s.host[0] && !fetch(s.host)) wait = RETRY_MS;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));  // un changement de réglages réveille la tâche
  }
}

void airBegin() {
  lock = xSemaphoreCreateMutex();
  Preferences prefs;
  prefs.begin("air");
  settings.enabled = prefs.getBool("on", settings.enabled);
  settings.bar = prefs.getBool("barre", settings.bar);
  if (prefs.isKey("hote")) prefs.getString("hote", settings.host, sizeof(settings.host));
  prefs.end();
  resp = (char *)heap_caps_malloc(RESP_SIZE, MALLOC_CAP_SPIRAM);
  // Pile en PSRAM : permis parce que la tâche n'écrit jamais en flash (voir weather.cpp)
  if (resp) xTaskCreatePinnedToCoreWithCaps(airTask, "air", 6144, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);
}

bool airGet(float &pm25, float &pm10) {
  if (!settings.enabled || !settings.host[0] || !valid || millis() - read_at > MAX_AGE_MS) return false;
  pm25 = last_pm25;
  pm10 = last_pm10;
  return true;
}

void airSettings(AirSettings &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = settings;
  xSemaphoreGive(lock);
}

void airConfigure(const AirSettings &s) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool moved = strcmp(s.host, settings.host) != 0, changed = moved || s.enabled != settings.enabled || s.bar != settings.bar;
  settings = s;
  if (moved) valid = false;
  xSemaphoreGive(lock);
  if (!changed) return;
  Preferences prefs;
  prefs.begin("air");
  prefs.putBool("on", s.enabled);
  prefs.putBool("barre", s.bar);
  prefs.putString("hote", s.host);
  prefs.end();
  if (task) xTaskNotifyGive(task);
}
