#include "space.h"
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/idf_additions.h>
#include "json.h"
#include "net.h"

#define HOST             "corquaid.github.io"
#define PATH             "/international-space-station-APIs/JSON/people-in-space.json"
#define HTTP_TIMEOUT_MS  8000
#define FIRST_READ_MS    35000                    // pas de lecture juste après le démarrage : comme crypto.cpp
#define REFRESH_MS       (6UL * 60 * 60 * 1000)   // un équipage change quelques fois par an
#define RETRY_MS         (10UL * 60 * 1000)       // après un échec
#define RESP_SIZE        2048                     // le nombre est au début de la réponse : inutile de tout lire

static volatile int people = -1;
static volatile bool enabled = true;
static TaskHandle_t task;
static char *resp;  // début de la réponse HTTP, en PSRAM

static bool fetch() {
  NetHttps one_at_a_time;
  NetworkClientSecure tls;
  tls.setInsecure();  // certificat non vérifié : rien de secret n'est envoyé, le nombre reçu n'est qu'affiché
  if (!tls.connect(HOST, 443, HTTP_TIMEOUT_MS)) return false;
  tls.print("GET " PATH " HTTP/1.0\r\nHost: " HOST "\r\nUser-Agent: ecran-esp32\r\n\r\n");
  size_t len = 0;
  uint32_t t0 = millis();
  const char *number = nullptr;
  resp[0] = 0;
  // {"number": 14, ...} : on s'arrête dès que le nombre est là
  while (len < RESP_SIZE - 1 && millis() - t0 < HTTP_TIMEOUT_MS && (tls.connected() || tls.available())) {
    int got = tls.read((uint8_t *)resp + len, RESP_SIZE - 1 - len);
    if (got <= 0) {
      delay(5);
      continue;
    }
    len += got;
    resp[len] = 0;
    number = jsonValue(strstr(resp, "\r\n\r\n"), "number");
    if (number && strpbrk(number, ",}")) break;
  }
  tls.stop();
  if (!number || !strstr(resp, " 200 ")) return false;
  int count = atoi(number);
  if (count <= 0 || count > 99) return false;
  people = count;
  Serial.printf("Espace : %d personnes\n", count);
  return true;
}

static void spaceTask(void *) {
  delay(FIRST_READ_MS);
  for (;;) {
    uint32_t wait = 500;  // en attendant le Wi-Fi
    if (!enabled) wait = REFRESH_MS;
    else if (WiFi.status() == WL_CONNECTED) wait = fetch() ? REFRESH_MS : RETRY_MS;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));  // l'activation réveille la tâche
  }
}

void spaceBegin() {
  Preferences prefs;
  prefs.begin("ecran");
  enabled = prefs.getBool("espace", enabled);
  prefs.end();
  resp = (char *)heap_caps_malloc(RESP_SIZE, MALLOC_CAP_SPIRAM);
  // Pile en PSRAM : permis parce que la tâche n'écrit jamais en flash (voir weather.cpp)
  if (resp) xTaskCreatePinnedToCoreWithCaps(spaceTask, "espace", 12288, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);
}

int spacePeople() {
  return enabled ? people : -1;
}

bool spaceEnabled() {
  return enabled;
}

void spaceSetEnabled(bool on) {
  if (on == enabled) return;
  enabled = on;
  Preferences prefs;
  prefs.begin("ecran");
  prefs.putBool("espace", on);
  prefs.end();
  if (on && task) xTaskNotifyGive(task);
}
