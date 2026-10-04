#include "net.h"
#include <WiFi.h>
#include <esp_sntp.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copier include/secrets.example.h en include/secrets.h et y mettre les identifiants Wi-Fi"
#endif

// Europe/Bruxelles, passage à l'heure d'été automatique
#define TIMEZONE    "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER  "pool.ntp.org"

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
        configTzTime(TIMEZONE, NTP_SERVER);
      },
      ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // la reconnexion est ensuite automatique
  vTaskDelete(nullptr);
}

void netBegin() {
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
