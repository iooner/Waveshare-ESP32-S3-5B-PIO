#include "crypto.h"
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/idf_additions.h>
#include "json.h"
#include "net.h"

#define HOST             "api.coingecko.com"
#define HTTP_TIMEOUT_MS  8000
#define FIRST_READ_MS    30000                // pas de lecture juste après le démarrage : voir cryptoTask()
#define REFRESH_MS       (2UL * 60 * 1000)    // l'accès sans clé de CoinGecko est limité à quelques lectures par minute
#define RETRY_MS         (60UL * 1000)        // après un échec
#define MAX_AGE_MS       (20UL * 60 * 1000)   // au-delà, mieux vaut ne rien afficher que des cours périmés
#define RESP_SIZE        12288                // la réponse fait ~1 Ko par crypto, plus 1 Ko d'en-têtes

static Crypto current;  // cryptos choisies et leurs cours
static uint32_t version = 0, fetched_at;
static bool valid = false;  // les cours sont ceux des cryptos choisies
static SemaphoreHandle_t lock;
static TaskHandle_t task;
static char *resp;  // réponse HTTP en cours, en PSRAM

// Lit les cours et les publie. Faux si le serveur ne répond pas.
static bool fetch() {
  Crypto c;
  cryptoGet(c);
  if (c.count == 0) return true;

  NetHttps one_at_a_time;
  NetworkClientSecure tls;
  // Le certificat du serveur n'est pas vérifié : rien de secret n'est envoyé, et ce qui est reçu
  // n'est qu'affiché. Un tiers sur le chemin pourrait au pire fausser les cours à l'écran.
  tls.setInsecure();
  if (!tls.connect(HOST, 443, HTTP_TIMEOUT_MS)) return false;
  // HTTP/1.0 : la réponse arrive d'un bloc, puis le serveur ferme la connexion
  tls.print("GET /api/v3/coins/markets?vs_currency=eur&price_change_percentage=1h,24h,7d&ids=");
  for (uint8_t i = 0; i < c.count; i++) tls.printf("%s%s", i ? "," : "", c.coins[i].id);
  tls.print(" HTTP/1.0\r\nHost: " HOST "\r\nUser-Agent: ecran-esp32\r\nAccept: application/json\r\n\r\n");
  size_t len = 0;
  uint32_t t0 = millis();
  while (len < RESP_SIZE - 1 && millis() - t0 < HTTP_TIMEOUT_MS && (tls.connected() || tls.available())) {
    int got = tls.read((uint8_t *)resp + len, RESP_SIZE - 1 - len);
    if (got > 0) len += got;
    else delay(5);
  }
  resp[len] = 0;
  tls.stop();
  const char *body = strstr(resp, "\r\n\r\n");
  if (!body || !strstr(resp, " 200 ")) return false;

  // [{"id":"bitcoin",...,"current_price":76736,...,"price_change_percentage_1h_in_currency":0.27,...},...]
  // Une crypto inconnue est absente ; une variation inconnue vaut null.
  static const char *const CHANGES[] = {"price_change_percentage_1h_in_currency", "price_change_percentage_24h_in_currency",
                                        "price_change_percentage_7d_in_currency"};
  for (uint8_t i = 0; i < c.count; i++) {
    char id[64];
    snprintf(id, sizeof(id), "\"id\":\"%s\"", c.coins[i].id);
    const char *coin = strstr(body, id), *price = jsonValue(coin, "current_price");
    c.price[i] = price ? atof(price) : 0;
    for (uint8_t k = 0; k < 3; k++) {
      const char *change = jsonValue(coin, CHANGES[k]);
      c.change[i][k] = change && *change != 'n' ? atof(change) : NAN;
    }
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  // Les réglages ont pu changer pendant la lecture : ces cours ne seraient pas ceux des bonnes cryptos
  bool same = c.count == current.count;
  for (uint8_t i = 0; same && i < c.count; i++) same = strcmp(c.coins[i].id, current.coins[i].id) == 0;
  if (same) {
    memcpy(current.price, c.price, sizeof(c.price));
    memcpy(current.change, c.change, sizeof(c.change));
    valid = true;
    version++;
    fetched_at = millis();
  }
  xSemaphoreGive(lock);
  Serial.printf("Crypto : %u cours lus\n", c.count);
  return true;
}

static void cryptoTask(void *) {
  // Première lecture différée : si la connexion chiffrée faisait planter la carte, il resterait
  // après chaque redémarrage de quoi lui envoyer un autre firmware par Wi-Fi
  delay(FIRST_READ_MS);
  for (;;) {
    uint32_t wait = 500;  // en attendant le Wi-Fi
    if (WiFi.status() == WL_CONNECTED) {
      bool ok = fetch();
      if (!ok) Serial.println("Crypto : pas de réponse");
      wait = ok ? REFRESH_MS : RETRY_MS;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));  // un changement de réglages réveille la tâche
  }
}

void cryptoBegin() {
  lock = xSemaphoreCreateMutex();
  Preferences prefs;
  prefs.begin("crypto");
  uint8_t count = prefs.getUChar("n", 0);
  if (count <= CRYPTO_MAX && prefs.getBytes("cryptos", current.coins, sizeof(current.coins)) == sizeof(current.coins)) {
    current.count = count;
  }
  prefs.end();

  resp = (char *)heap_caps_malloc(RESP_SIZE, MALLOC_CAP_SPIRAM);
  // Pile en PSRAM, pour garder la RAM interne à l'écran : permis parce que la tâche n'écrit jamais
  // en flash (les réglages sont enregistrés par la tâche qui appelle cryptoConfigure())
  if (resp) xTaskCreatePinnedToCoreWithCaps(cryptoTask, "crypto", 12288, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);
}

uint32_t cryptoVersion() {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = current.count && valid && millis() - fetched_at < MAX_AGE_MS ? version : 0;
  xSemaphoreGive(lock);
  return v;
}

void cryptoGet(Crypto &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = current;
  xSemaphoreGive(lock);
}

int32_t cryptoAge() {
  xSemaphoreTake(lock, portMAX_DELAY);
  int32_t age = valid ? (millis() - fetched_at) / 1000 : -1;
  xSemaphoreGive(lock);
  return age;
}

void cryptoConfigure(const CryptoCoin *coins, uint8_t count) {
  Crypto n = {};
  n.count = min<uint8_t>(count, CRYPTO_MAX);
  memcpy(n.coins, coins, n.count * sizeof(CryptoCoin));

  xSemaphoreTake(lock, portMAX_DELAY);
  bool same_coins = n.count == current.count;
  for (uint8_t i = 0; same_coins && i < n.count; i++) same_coins = strcmp(n.coins[i].id, current.coins[i].id) == 0;
  bool changed = !same_coins || memcmp(n.coins, current.coins, sizeof(n.coins)) != 0;
  if (changed) {
    // Mêmes cryptos, autres quantités : les cours restent bons, la page se redessine
    memcpy(current.coins, n.coins, sizeof(n.coins));
    current.count = n.count;
    if (same_coins) version++;
    else valid = false;
  }
  xSemaphoreGive(lock);
  if (!changed) return;

  Preferences prefs;
  prefs.begin("crypto");
  prefs.putUChar("n", n.count);
  prefs.putBytes("cryptos", n.coins, sizeof(n.coins));
  prefs.end();
  if (!same_coins && task) xTaskNotifyGive(task);
}
