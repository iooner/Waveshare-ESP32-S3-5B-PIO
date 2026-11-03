#include "stocks.h"
#include <NetworkClientSecure.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/idf_additions.h>
#include "json.h"
#include "net.h"
#include "text.h"

#define HOST             "www.ls-tc.de"
#define HTTP_TIMEOUT_MS  10000
#define FIRST_READ_MS    35000                // après la crypto : voir cryptoTask() dans crypto.cpp
#define REFRESH_MS       (5UL * 60 * 1000)    // pendant les heures de bourse
#define CLOSED_MS        (60UL * 60 * 1000)   // la nuit et le week-end : les cours ne bougent plus
#define RETRY_MS         (60UL * 1000)        // après un échec
#define MAX_AGE_MS       (3UL * 60 * 60 * 1000)  // au-delà, mieux vaut ne rien afficher que des cours périmés
#define HEAD_SIZE        2048                 // début d'une réponse : en-têtes, puis la clôture de la veille
#define TAIL_SIZE        256                  // fin d'une réponse : le dernier cours

static Stocks *current;  // titres choisis et leurs cours, en PSRAM
static uint32_t version = 0, fetched_at;
static bool valid = false;  // les cours sont ceux des titres choisis
static uint8_t unknown = 0;
static SemaphoreHandle_t lock;
static TaskHandle_t task;
static char *head, *tail;  // réponse en cours, en PSRAM

// Correspondance entre un ISIN et le numéro du titre chez Lang & Schwarz, cherchée une fois. Seule
// la tâche de lecture s'en sert.
struct Known {
  char isin[13];
  int32_t id;  // 0 : inconnu de la bourse
  char title[24];
};
static Known *known;  // STOCKS_MAX, en PSRAM
static uint8_t known_next;
static volatile bool dirty;  // titres changés, à enregistrer par stocksLoop()

#define FILE_PATH  "/actions.bin"  // les titres, à la suite, dans la partition de fichiers (250 font 12 Ko)

// Envoie une requête et garde le début et la fin de la réponse, qui peut faire plus de 20 Ko.
// Faux si le serveur ne répond pas ou pas par 200.
static bool get(const char *path) {
  // Une connexion à la fois, et rendue entre deux titres : avec beaucoup de titres, un tour de lecture
  // dure plusieurs minutes, et la météo, la crypto et les agendas passent entre deux
  NetHttps one_at_a_time;
  NetworkClientSecure tls;
  // Le certificat du serveur n'est pas vérifié : rien de secret n'est envoyé, et ce qui est reçu
  // n'est qu'affiché. Un tiers sur le chemin pourrait au pire fausser les cours à l'écran.
  tls.setInsecure();
  if (!tls.connect(HOST, 443, HTTP_TIMEOUT_MS)) return false;
  // HTTP/1.0 : la réponse arrive d'un bloc, puis le serveur ferme la connexion
  tls.printf("GET %s HTTP/1.0\r\nHost: " HOST "\r\nUser-Agent: ecran-esp32\r\nAccept: application/json\r\n\r\n", path);
  size_t head_len = 0, tail_len = 0;
  uint8_t chunk[512];
  uint32_t t0 = millis();
  while (millis() - t0 < HTTP_TIMEOUT_MS && (tls.connected() || tls.available())) {
    int got = tls.read(chunk, sizeof(chunk));
    if (got <= 0) {
      delay(5);
      continue;
    }
    size_t take = min<size_t>(got, HEAD_SIZE - 1 - head_len);
    memcpy(head + head_len, chunk, take);
    head_len += take;
    // La fin : ce qui restait, décalé, puis ce morceau
    size_t keep = min<size_t>(tail_len, TAIL_SIZE - 1 - min<size_t>(got, TAIL_SIZE - 1));
    memmove(tail, tail + tail_len - keep, keep);
    size_t add = min<size_t>(got, TAIL_SIZE - 1);
    memcpy(tail + keep, chunk + got - add, add);
    tail_len = keep + add;
  }
  head[head_len] = 0;
  tail[tail_len] = 0;
  tls.stop();
  return strncmp(head, "HTTP/1.", 7) == 0 && strncmp(head + 8, " 200", 4) == 0;
}

// Copie une chaîne JSON qui commence à `from` (après son guillemet), échappements compris
static void jsonString(const char *from, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = from; p && *p && *p != '"' && n < cap - 3; p++) {
    if (*p != '\\') {
      out[n++] = *p;
    } else if (p[1] == 'u' && isxdigit((uint8_t)p[2])) {
      // é : en UTF-8, s'il est dans le Latin-1 des polices
      char hex[5] = {p[2], p[3], p[4], p[5], 0};
      long c = strtol(hex, nullptr, 16);
      if (c >= 0x80 && c < 0x100) out[n++] = 0xC0 | c >> 6, out[n++] = 0x80 | (c & 0x3F);
      else if (c >= 0x20 && c < 0x80) out[n++] = c;
      p += 5;
    } else if (p[1]) {
      out[n++] = *++p;
    }
  }
  out[n] = 0;
}

// Numéro du titre chez Lang & Schwarz, cherché par son ISIN. -1 si la bourse ne répond pas.
static int32_t lookup(const char *isin, char *title, size_t cap) {
  for (uint8_t i = 0; i < STOCKS_MAX; i++) {
    const Known &k = known[i];
    if (strcmp(k.isin, isin) == 0) {
      strlcpy(title, k.title, cap);
      return k.id;
    }
  }
  char path[96];
  snprintf(path, sizeof(path), "/_rpc/json/.lstc/instrument/search/main?q=%s&localeId=2", isin);
  if (!get(path)) return -1;
  // [{"id":1234,"displayname":"SIEMENS AG","isin":"DE0007236101",...,"instrumentId":1234,...},...]
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"isin\":\"%s\"", isin);
  const char *found = strstr(head, pattern), *start = found;
  while (start && start > head && *start != '{') start--;
  const char *id = jsonValue(start, "instrumentId"), *name = jsonValue(start, "displayname");
  auto &k = known[known_next];
  known_next = (known_next + 1) % STOCKS_MAX;
  strlcpy(k.isin, isin, sizeof(k.isin));
  k.id = found && id ? atol(id) : 0;
  k.title[0] = 0;
  if (k.id && name && *name == '"') {
    jsonString(name + 1, k.title, sizeof(k.title));
    // Les noms de la bourse sont alignés par des espaces : "SIEMENS AG      NA O.N."
    char *w = k.title;
    for (const char *r = k.title; *r; r++) {
      if (*r != ' ' || (w > k.title && w[-1] != ' ')) *w++ = *r;
    }
    *w = 0;
    toLatin1(k.title);
    dropUnsupported(k.title);
  }
  strlcpy(title, k.title, cap);
  return k.id;
}

// Dernier cours et clôture de la veille d'un titre. Faux si la bourse ne répond pas.
static bool quote(int32_t id, double &price, double &previous) {
  char path[112];
  snprintf(path, sizeof(path), "/_rpc/json/instrument/chart/dataForInstrument?instrumentId=%ld&series=intraday", (long)id);
  if (!get(path)) return false;
  // Au début : {"info":{...,"plotlines":[{"label":"Vortag 100,50","value":100.5,...,"id":"previousDay"}]}
  const char *lines = strstr(head, "\"plotlines\"");
  const char *value = lines && strstr(lines, "previousDay") ? jsonValue(lines, "value") : nullptr;
  previous = value ? atof(value) : 0;
  // A la fin, les cours de la journée, minute par minute : ...,[1793709692000,101.25]],"name":...
  price = 0;
  char *end = nullptr;
  for (char *p = strstr(tail, "]]"); p; p = strstr(p + 1, "]]")) end = p;
  if (end) {
    char *comma = end;
    while (comma > tail && *comma != ',' && *comma != '[') comma--;
    if (*comma == ',' && isdigit((uint8_t)comma[-1])) price = atof(comma + 1);
  }
  // Pas encore d'échange aujourd'hui : le cours est celui de la clôture
  if (price <= 0) price = previous;
  return true;
}

// Lit les cours et les publie. Faux si le serveur ne répond pas.
static bool fetch() {
  static Stocks &s = *(Stocks *)heap_caps_malloc(sizeof(Stocks), MALLOC_CAP_SPIRAM);  // seule cette tâche s'en sert
  stocksGet(s);
  if (s.count == 0) return true;

  uint8_t missing = 0;
  for (uint8_t i = 0; i < s.count; i++) {
    s.price[i] = s.previous[i] = 0;
    int32_t id = lookup(s.stocks[i].isin, s.title[i], sizeof(s.title[i]));
    if (id < 0) return false;
    if (id == 0) {
      missing++;
      continue;
    }
    if (!quote(id, s.price[i], s.previous[i])) return false;
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  // Les réglages ont pu changer pendant la lecture : ces cours ne seraient pas ceux des bons titres
  bool same = s.count == current->count;
  for (uint8_t i = 0; same && i < s.count; i++) same = strcmp(s.stocks[i].isin, current->stocks[i].isin) == 0;
  if (same) {
    memcpy(current->title, s.title, sizeof(s.title));
    memcpy(current->price, s.price, sizeof(s.price));
    memcpy(current->previous, s.previous, sizeof(s.previous));
    unknown = missing;
    valid = true;
    version++;
    fetched_at = millis();
  }
  xSemaphoreGive(lock);
  Serial.printf("Actions : %u cours lus, %u ISIN inconnus\n", s.count - missing, missing);
  return true;
}

// Heures d'ouverture de Lang & Schwarz, larges : 7 h 30 à 23 h, heure de Bruxelles, en semaine
static bool marketOpen() {
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  int minute = t.tm_hour * 60 + t.tm_min;
  return t.tm_wday >= 1 && t.tm_wday <= 5 && minute >= 5 * 60 && minute <= 22 * 60 + 30;
}

static void stocksTask(void *) {
  delay(FIRST_READ_MS);
  for (;;) {
    uint32_t wait = 500;  // en attendant le Wi-Fi
    if (WiFi.status() == WL_CONNECTED) {
      bool ok = fetch();
      if (!ok) Serial.println("Actions : pas de réponse");
      wait = !ok ? RETRY_MS : marketOpen() ? REFRESH_MS : CLOSED_MS;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));  // un changement de réglages réveille la tâche
  }
}

void stocksBegin() {
  lock = xSemaphoreCreateMutex();
  current = (Stocks *)heap_caps_calloc(1, sizeof(Stocks), MALLOC_CAP_SPIRAM);
  known = (Known *)heap_caps_calloc(STOCKS_MAX, sizeof(Known), MALLOC_CAP_SPIRAM);
  // La partition est formatée au premier démarrage qui en a besoin
  if (LittleFS.begin(true)) {
    File f = LittleFS.open(FILE_PATH, "r");
    if (f) {
      size_t len = f.size() - f.size() % sizeof(Stock);
      len = min(len, sizeof(current->stocks));
      if (f.read((uint8_t *)current->stocks, len) == len) current->count = len / sizeof(Stock);
      f.close();
    } else {
      // Version précédente : les titres étaient dans les réglages, trop petits pour en garder beaucoup
      Preferences prefs;
      prefs.begin("actions");
      uint8_t count = prefs.getUChar("n", 0);
      size_t len = prefs.getBytesLength("titres");
      if (count && len <= sizeof(current->stocks) && len % sizeof(Stock) == 0 && count * sizeof(Stock) <= len &&
          prefs.getBytes("titres", current->stocks, len) == len) {
        current->count = count;
        dirty = true;  // passe dans le fichier ; les réglages sont vidés une fois le fichier écrit
      }
      prefs.end();
    }
  } else {
    Serial.println("Actions : partition de fichiers inutilisable");
  }

  head = (char *)heap_caps_malloc(HEAD_SIZE + TAIL_SIZE, MALLOC_CAP_SPIRAM);
  tail = head + HEAD_SIZE;
  // Pile en PSRAM, pour garder la RAM interne à l'écran : permis parce que la tâche n'écrit jamais
  // en flash (les réglages sont enregistrés par la tâche qui appelle stocksConfigure())
  if (head) xTaskCreatePinnedToCoreWithCaps(stocksTask, "actions", 12288, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);
}

uint32_t stocksVersion() {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = current->count && valid && millis() - fetched_at < MAX_AGE_MS ? version : 0;
  xSemaphoreGive(lock);
  return v;
}

void stocksGet(Stocks &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = *current;
  xSemaphoreGive(lock);
}

int32_t stocksAge() {
  xSemaphoreTake(lock, portMAX_DELAY);
  int32_t age = valid ? (millis() - fetched_at) / 1000 : -1;
  xSemaphoreGive(lock);
  return age;
}

uint8_t stocksUnknown() {
  return valid ? unknown : 0;
}

void stocksConfigure(const Stock *stocks, uint8_t count) {
  static Stock *n = (Stock *)heap_caps_malloc(STOCKS_MAX * sizeof(Stock), MALLOC_CAP_SPIRAM);  // une seule tâche passe ici
  memset(n, 0, STOCKS_MAX * sizeof(Stock));
  count = min<uint8_t>(count, STOCKS_MAX);
  memcpy(n, stocks, count * sizeof(Stock));

  xSemaphoreTake(lock, portMAX_DELAY);
  bool same_isins = count == current->count;
  for (uint8_t i = 0; same_isins && i < count; i++) same_isins = strcmp(n[i].isin, current->stocks[i].isin) == 0;
  bool changed = !same_isins || memcmp(n, current->stocks, STOCKS_MAX * sizeof(Stock)) != 0;
  if (changed) {
    // Mêmes titres, autres quantités ou noms : les cours restent bons, la page se redessine
    memcpy(current->stocks, n, STOCKS_MAX * sizeof(Stock));
    current->count = count;
    if (same_isins) version++;
    else valid = false;
  }
  xSemaphoreGive(lock);
  if (!changed) return;
  dirty = true;
  if (!same_isins && task) xTaskNotifyGive(task);
}

void stocksLoop() {
  if (!dirty) return;
  dirty = false;
  static Stock *copy = (Stock *)heap_caps_malloc(STOCKS_MAX * sizeof(Stock), MALLOC_CAP_SPIRAM);  // seule la boucle passe ici
  xSemaphoreTake(lock, portMAX_DELAY);
  uint8_t count = current->count;
  memcpy(copy, current->stocks, count * sizeof(Stock));
  xSemaphoreGive(lock);
  File f = LittleFS.open(FILE_PATH, "w");
  bool ok = f && f.write((const uint8_t *)copy, count * sizeof(Stock)) == count * sizeof(Stock);
  if (f) f.close();
  if (!ok) {
    Serial.println("Actions : enregistrement impossible");
    return;
  }
  Preferences prefs;
  prefs.begin("actions");
  if (prefs.isKey("titres")) prefs.clear();
  prefs.end();
}
