#include "agenda.h"
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <freertos/idf_additions.h>
#include "net.h"
#include "text.h"

#define HOST             "lghs.be"
#define PATH             "/calendar.php"
#define HTTP_TIMEOUT_MS  15000
#define FIRST_READ_MS    40000                    // pas de lecture juste après le démarrage : comme crypto.cpp
#define REFRESH_MS       (60UL * 60 * 1000)
#define RETRY_MS         (10UL * 60 * 1000)       // après un échec
#define MAX_AGE_MS       (48UL * 60 * 60 * 1000)  // au-delà, le calendrier a pu changer : mieux vaut ne rien afficher
#define UPDATE_MS        60000                    // la liste dépend de l'heure : un événement fini en sort
#define FEED_SIZE        (96 * 1024)              // le calendrier fait ~22 Ko

static Agenda current;
static uint32_t version = 0, fetched_at;
static bool have_feed = false;
static SemaphoreHandle_t lock;
static char *feed;  // le calendrier, lignes dépliées, en PSRAM

// Télécharge le calendrier. Faux si le serveur ne répond pas : le précédent est alors gardé.
static bool fetch() {
  NetworkClientSecure tls;
  tls.setInsecure();  // certificat non vérifié : rien de secret n'est envoyé, l'agenda reçu n'est qu'affiché
  if (!tls.connect(HOST, 443, HTTP_TIMEOUT_MS)) return false;
  tls.print("GET " PATH " HTTP/1.0\r\nHost: " HOST "\r\nUser-Agent: ecran-esp32\r\n\r\n");
  // Lu dans la seconde moitié du tampon, pour ne remplacer le calendrier en place qu'une fois complet
  char *resp = feed + FEED_SIZE / 2;
  size_t len = 0, cap = FEED_SIZE / 2 - 1;
  uint32_t last = millis();
  while (len < cap && millis() - last < HTTP_TIMEOUT_MS && (tls.connected() || tls.available())) {
    int got = tls.read((uint8_t *)resp + len, cap - len);
    if (got > 0) len += got, last = millis();
    else delay(5);
  }
  resp[len] = 0;
  tls.stop();
  const char *body = strstr(resp, "\r\n\r\n");
  if (!body || !strstr(resp, " 200 ") || !strstr(body, "END:VCALENDAR")) return false;  // incomplet : on garde l'ancien
  memmove(feed, body + 4, strlen(body + 4) + 1);
  icalUnfold(feed);
  have_feed = true;
  fetched_at = millis();
  Serial.printf("Agenda : calendrier lu, %u octets\n", (unsigned)len);
  return true;
}

// Refait la liste des prochains événements et la publie si elle a changé
static void update() {
  if (!have_feed || !netTimeSynced()) return;
  Agenda a = {};
  a.count = icalUpcoming(feed, time(nullptr), a.events, AGENDA_MAX);
  for (uint8_t i = 0; i < a.count; i++) toLatin1(a.events[i].title);
  xSemaphoreTake(lock, portMAX_DELAY);
  if (memcmp(&a, &current, sizeof(a)) != 0) {
    current = a;
    version++;
  }
  xSemaphoreGive(lock);
}

static void agendaTask(void *) {
  delay(FIRST_READ_MS);
  uint32_t next_fetch = 0;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED && (int32_t)(millis() - next_fetch) >= 0) {
      bool ok = fetch();
      if (!ok) Serial.println("Agenda : pas de réponse");
      next_fetch = millis() + (ok ? REFRESH_MS : RETRY_MS);
    }
    update();
    delay(UPDATE_MS);
  }
}

void agendaBegin() {
  lock = xSemaphoreCreateMutex();
  feed = (char *)heap_caps_malloc(FEED_SIZE, MALLOC_CAP_SPIRAM);
  // Pile en PSRAM : permis parce que la tâche n'écrit jamais en flash (voir weather.cpp)
  if (feed) xTaskCreatePinnedToCoreWithCaps(agendaTask, "agenda", 16384, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
}

uint32_t agendaVersion() {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = current.count && have_feed && millis() - fetched_at < MAX_AGE_MS ? version : 0;
  xSemaphoreGive(lock);
  return v;
}

void agendaGet(Agenda &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = current;
  xSemaphoreGive(lock);
}

int32_t agendaAge() {
  return have_feed ? (millis() - fetched_at) / 1000 : -1;
}
