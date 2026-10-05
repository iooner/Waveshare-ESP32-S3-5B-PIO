#include "net.h"
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <mbedtls/platform.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copier include/secrets.example.h en include/secrets.h et y mettre les identifiants Wi-Fi"
#endif

// Europe/Bruxelles, passage à l'heure d'été automatique
#define TIMEZONE    "CET-1CEST,M3.5.0,M10.5.0/3"  // NET_TIMEZONE_NAME en notation POSIX
#define NTP_SERVER  "pool.ntp.org"

static char tz_rule[32] = TIMEZONE;                // fuseau en place, en notation POSIX
static int32_t utc_offset = NET_TIMEZONE_DEFAULT;  // le même, tel que demandé à netSetTimezone()
static volatile bool unsaved = false;

static void onTimeSync(struct timeval *) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
  Serial.printf("Heure reçue : %s\n", buf);
}

// Le Wi-Fi est démarré depuis le coeur 0 : ses interruptions restent loin de celles du LCD,
// qui sont sur le coeur 1
static void netTask(void *) {
  // Sans ça, les identifiants sont écrits en flash à chaque connexion. Une écriture en flash
  // coupe le cache, donc l'accès à la PSRAM : l'image saute le temps de l'écriture.
  WiFi.persistent(false);
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t) {
        Serial.printf("Wi-Fi connecté : %s\n", WiFi.localIP().toString().c_str());
        // Relancé à chaque connexion : la requête NTP part tout de suite, puis toutes les heures
        configTzTime(tz_rule, NTP_SERVER);
      },
      ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // la reconnexion est ensuite automatique
  vTaskDelete(nullptr);
}

void netSetTimezone(int32_t offset) {
  if (offset == utc_offset) return;
  utc_offset = offset;
  if (offset == NET_TIMEZONE_DEFAULT) {
    strlcpy(tz_rule, TIMEZONE, sizeof(tz_rule));
  } else {
    // Notation POSIX : un nom entre chevrons, puis le décalage à l'envers (UTC+2 s'écrit -2)
    long minutes = labs(offset) / 60;
    snprintf(tz_rule, sizeof(tz_rule), "<%c%02ld%02ld>%c%ld:%02ld", offset < 0 ? '-' : '+', minutes / 60, minutes % 60,
             offset < 0 ? '+' : '-', minutes / 60, minutes % 60);
  }
  setenv("TZ", tz_rule, 1);
  tzset();
  unsaved = true;
  Serial.printf("Fuseau horaire : %s\n", tz_rule);
}

// L'écriture en flash se fait ici et pas dans netSetTimezone() : celle-ci est appelée par la
// tâche météo, dont la pile est en PSRAM, et une telle tâche ne peut pas écrire en flash
void netLoop() {
  if (!unsaved) return;
  unsaved = false;
  Preferences prefs;
  prefs.begin("reseau");
  prefs.putInt("utc", utc_offset);
  prefs.end();
}

// Mémoire du chiffrement (TLS) : en PSRAM. Une connexion HTTPS demande ~40 Ko, que le SDK
// prendrait en RAM interne, où il n'en reste pas assez à côté de l'écran.
static void *tlsCalloc(size_t n, size_t size) {
  return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void netBegin() {
  mbedtls_platform_set_calloc_free(tlsCalloc, heap_caps_free);
  Preferences prefs;
  prefs.begin("reseau");
  netSetTimezone(prefs.getInt("utc", NET_TIMEZONE_DEFAULT));
  prefs.end();
  unsaved = false;
  sntp_set_time_sync_notification_cb(onTimeSync);
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
}

bool netConnected() {
  return WiFi.status() == WL_CONNECTED;
}

bool netTimeSynced() {
  // L'horloge interne démarre en 1970 : avant 2023, rien n'a encore été reçu
  return time(nullptr) > 1700000000;
}
