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

// Le Wi-Fi allumé perturbe l'écran et le rétroéclairage. Mesuré sur la carte : 1 à 4 images
// ratées par seconde et des impulsions de rétroéclairage jusqu'à 3 fois trop longues, contre
// aucune image ratée Wi-Fi éteint. On ne l'allume donc que le temps de recevoir l'heure,
// puis l'horloge interne prend le relais jusqu'au recalage suivant.
#define SYNC_TIMEOUT_MS  20000  // durée maximale d'une tentative
#define RESYNC_HOUR      4      // recalage quotidien à 4 h : personne ne regarde l'écran

// Attente avant un nouvel essai après un échec, de plus en plus longue : réseau en panne,
// l'écran n'est plus perturbé que 20 s par heure
static const uint16_t RETRY_MINUTES[] = {5, 15, 60};
#define RETRY_STEPS  (sizeof(RETRY_MINUTES) / sizeof(RETRY_MINUTES[0]))

static volatile bool time_received = false;  // heure reçue pendant la tentative en cours

static uint32_t msUntilResync() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int32_t secs = (RESYNC_HOUR - t.tm_hour + 24) % 24 * 3600 - t.tm_min * 60 - t.tm_sec;
  if (secs <= 0) secs += 24 * 3600;
  return secs * 1000UL;
}

static void onTimeSync(struct timeval *) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
  Serial.printf("Heure reçue : %s\n", buf);
  time_received = true;
}

// Tâche à part sur le coeur 0 : allumer et éteindre le Wi-Fi prend du temps, la boucle
// d'affichage ne doit pas attendre dessus.
static void netTask(void *) {
  // Sans ça, les identifiants sont écrits en flash à chaque connexion. Une écriture en flash
  // coupe le cache, donc l'accès à la PSRAM : l'image saute le temps de l'écriture.
  WiFi.persistent(false);
  WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t) {
        Serial.printf("Wi-Fi connecté : %s\n", WiFi.localIP().toString().c_str());
        // Relancé à chaque connexion : la requête NTP part tout de suite
        configTzTime(TIMEZONE, NTP_SERVER);
      },
      ARDUINO_EVENT_WIFI_STA_GOT_IP);

  uint8_t failures = 0;
  for (;;) {
    time_received = false;
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (!time_received && millis() - start < SYNC_TIMEOUT_MS) delay(100);

    bool ok = time_received;
    esp_sntp_stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    uint32_t wait;
    if (ok) {
      failures = 0;
      wait = msUntilResync();
    } else {
      wait = RETRY_MINUTES[failures] * 60000UL;
      if (failures < RETRY_STEPS - 1) failures++;
    }
    Serial.printf("%s, Wi-Fi éteint pendant %lu min\n", ok ? "Horloge à l'heure" : "Heure non reçue",
                  (unsigned long)(wait / 60000));
    delay(wait);
  }
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
