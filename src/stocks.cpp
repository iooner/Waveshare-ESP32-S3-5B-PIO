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
#define BATCH            25                   // titres lus sur une même connexion

static Stocks *current;  // titres choisis et leurs cours, en PSRAM
static uint32_t version = 0, fetched_at;
static bool valid = false;  // les cours sont ceux des titres choisis
static uint8_t unknown = 0;
static uint16_t round_seconds;  // durée du dernier tour de lecture
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
static volatile bool dirty;        // titres changés, à enregistrer par stocksLoop()
static volatile bool known_dirty;  // correspondances trouvées, idem : la tâche de lecture n'écrit pas en flash

#define FILE_PATH   "/actions.bin"  // les titres, à la suite, dans la partition de fichiers (250 font 12 Ko)
#define KNOWN_PATH  "/isin.bin"     // les correspondances trouvées : pas de recherche à chaque démarrage

// Connexion gardée ouverte d'un titre à l'autre pendant un tour de lecture (HTTP/1.1, keep-alive) :
// une seule négociation chiffrée pour tous les titres, au lieu d'une par titre. Seule la tâche de
// lecture s'en sert.
static NetworkClientSecure *tls;
static uint8_t *in;  // ce qui a été reçu et pas encore lu, en PSRAM
static size_t in_pos, in_len;
#define IN_SIZE  1024

// Prochain octet reçu, ou -1 si la connexion est coupée ou muette jusqu'à `deadline`
static int nextByte(uint32_t deadline) {
  while (in_pos == in_len) {
    int got = tls->read(in, IN_SIZE);
    if (got > 0) {
      in_pos = 0, in_len = got;
      break;
    }
    if (!tls->connected() || (int32_t)(millis() - deadline) > 0) return -1;
    delay(2);
  }
  return in[in_pos++];
}

// Une ligne d'en-tête ou de taille de morceau, sans son retour à la ligne. Faux si la connexion casse.
static bool readLine(char *out, size_t cap, uint32_t deadline) {
  size_t n = 0;
  for (int c; (c = nextByte(deadline)) >= 0;) {
    if (c == '\n') {
      while (n && out[n - 1] == '\r') n--;
      out[n] = 0;
      return true;
    }
    if (n < cap - 1) out[n++] = c;
  }
  return false;
}

// Garde le début (`head`) et la fin (`tail`) du corps de la réponse, qui peut faire plus de 20 Ko
static size_t head_len, tail_len;
static void keep(const uint8_t *p, size_t n) {
  size_t take = min<size_t>(n, HEAD_SIZE - 1 - head_len);
  memcpy(head + head_len, p, take);
  head_len += take;
  // La fin : ce qui restait, décalé, puis ce bloc
  size_t add = min<size_t>(n, TAIL_SIZE - 1), stay = min<size_t>(tail_len, TAIL_SIZE - 1 - add);
  memmove(tail, tail + tail_len - stay, stay);
  memcpy(tail + stay, p + n - add, add);
  tail_len = stay + add;
}

// Lit `n` octets du corps ; n = SIZE_MAX : jusqu'à la fermeture de la connexion
static bool readBody(size_t n, uint32_t deadline) {
  while (n) {
    if (in_pos == in_len && nextByte(deadline) >= 0) in_pos--;  // remplit le tampon sans rien consommer
    if (in_pos == in_len) return n == SIZE_MAX;
    size_t take = min(n, in_len - in_pos);
    keep(in + in_pos, take);
    in_pos += take;
    if (n != SIZE_MAX) n -= take;
  }
  return true;
}

static void disconnect() {
  if (tls) tls->stop();
  in_pos = in_len = 0;
}

// Envoie une requête et garde le début et la fin du corps de la réponse. Faux si le serveur ne
// répond pas ou pas par 200. La connexion reste ouverte pour la requête suivante.
static bool get(const char *path) {
  for (int attempt = 0; attempt < 2; attempt++) {
    // Connexion fermée par le serveur depuis la requête précédente : on en rouvre une, une fois
    if (!tls->connected()) {
      disconnect();
      if (!tls->connect(HOST, 443, HTTP_TIMEOUT_MS)) return false;
    }
    tls->printf("GET %s HTTP/1.1\r\nHost: " HOST "\r\nUser-Agent: ecran-esp32\r\nAccept: application/json\r\n\r\n", path);
    uint32_t deadline = millis() + HTTP_TIMEOUT_MS;
    char line[128];
    if (!readLine(line, sizeof(line), deadline)) {
      disconnect();
      continue;
    }
    bool ok = strncmp(line, "HTTP/1.", 7) == 0 && strncmp(line + 8, " 200", 4) == 0;
    // En-têtes : la façon dont le corps est délimité, et si la connexion reste ouverte
    bool chunked = false, close = false;
    size_t length = SIZE_MAX;
    while (readLine(line, sizeof(line), deadline) && line[0]) {
      if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strcasestr(line, "chunked")) chunked = true;
      else if (strncasecmp(line, "Content-Length:", 15) == 0) length = strtoul(line + 15, nullptr, 10);
      else if (strncasecmp(line, "Connection:", 11) == 0 && strcasestr(line, "close")) close = true;
    }
    head_len = tail_len = 0;
    bool complete;
    if (chunked) {
      // Des morceaux, chacun précédé de sa taille en hexadécimal ; le dernier est vide
      complete = false;
      while (readLine(line, sizeof(line), deadline)) {
        size_t n = strtoul(line, nullptr, 16);
        if (n == 0) {
          while (readLine(line, sizeof(line), deadline) && line[0]) {}  // en-têtes de fin, ligne vide
          complete = true;
          break;
        }
        if (!readBody(n, deadline) || !readLine(line, sizeof(line), deadline)) break;
      }
    } else {
      complete = readBody(length, deadline);
      close |= length == SIZE_MAX;
    }
    head[head_len] = 0;
    tail[tail_len] = 0;
    if (!complete || close) disconnect();
    if (complete) return ok;
  }
  return false;
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
    if (k.id && strcmp(k.isin, isin) == 0) {
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
  if (k.id) known_dirty = true;
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
  uint32_t t0 = millis();

  // Par lots : la connexion est rendue entre deux lots, pour que la météo, la crypto et les
  // agendas passent même quand il y a beaucoup de titres
  uint8_t missing = 0;
  for (uint8_t start = 0; start < s.count; start += BATCH) {
    NetHttps one_at_a_time;
    for (uint8_t i = start; i < s.count && i < start + BATCH; i++) {
      s.price[i] = s.previous[i] = 0;
      int32_t id = lookup(s.stocks[i].isin, s.title[i], sizeof(s.title[i]));
      if (id > 0 && quote(id, s.price[i], s.previous[i])) continue;
      if (id != 0) {
        disconnect();
        return false;
      }
      missing++;
    }
    disconnect();
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
    round_seconds = (millis() - t0 + 500) / 1000;
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

// Bourses principales, désignées par le pays de l'ISIN, et leurs heures de séance, en heure locale.
// Un ETF irlandais ou luxembourgeois se traite surtout à Francfort, Paris ou Amsterdam : il suit
// l'Europe. Les jours fériés ne sont pas connus.
enum Summer : uint8_t { SUMMER_NONE, SUMMER_EU, SUMMER_US };
struct Market {
  const char *countries;
  int16_t utc;          // décalage de l'heure d'hiver, en minutes
  Summer dst;           // heure d'été
  uint16_t open, close;  // en minutes depuis minuit
};
static const Market MARKETS[] = {
    {"US CA", -300, SUMMER_US, 9 * 60 + 30, 16 * 60},                                       // New York, Toronto
    {"GB", 0, SUMMER_EU, 8 * 60, 16 * 60 + 30},                                             // Londres
    {"FR BE NL PT DE AT CH IT ES IE LU DK SE NO FI", 60, SUMMER_EU, 9 * 60, 17 * 60 + 30},  // Euronext, Xetra...
    {"JP KR", 540, SUMMER_NONE, 9 * 60, 15 * 60 + 30},                                      // Tokyo, Séoul
    {"HK", 480, SUMMER_NONE, 9 * 60 + 30, 16 * 60},
    {"CN", 480, SUMMER_NONE, 9 * 60 + 30, 15 * 60},
    {"TW", 480, SUMMER_NONE, 9 * 60, 13 * 60 + 30},
    {"IN", 330, SUMMER_NONE, 9 * 60 + 15, 15 * 60 + 30},
};

// Jours depuis le 1er janvier 1970 (d'après H. Hinnant, « chrono-compatible date algorithms »)
static int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  uint32_t yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097 + (int32_t)(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
}

// Jour de la semaine, 0 = dimanche : le 1er janvier 1970 était un jeudi
static uint8_t weekday(int32_t y, uint32_t m, uint32_t d) {
  return (daysFromCivil(y, m, d) % 7 + 11) % 7;
}

// Jour du mois du n-ième dimanche, ou du dernier d'un mois de 31 jours
static uint8_t nthSunday(int32_t y, uint8_t m, uint8_t n) {
  return 1 + (7 - weekday(y, m, 1)) % 7 + 7 * (n - 1);
}
static uint8_t lastSunday(int32_t y, uint8_t m) {
  return 31 - weekday(y, m, 31);
}

int8_t stocksMarketOpen(const char *isin) {
  time_t now = time(nullptr);
  if (now < 1700000000 || strlen(isin) < 2) return -1;  // heure pas encore reçue
  char country[3] = {isin[0], isin[1], 0};
  for (const Market &m : MARKETS) {
    if (!strstr(m.countries, country)) continue;
    struct tm t;
    gmtime_r(&now, &t);
    int32_t year = t.tm_year + 1900;
    int64_t summer_from, summer_to;  // en secondes UTC
    if (m.dst == SUMMER_EU) {           // du dernier dimanche de mars au dernier d'octobre, à 1 h UTC
      summer_from = daysFromCivil(year, 3, lastSunday(year, 3)) * 86400LL + 3600;
      summer_to = daysFromCivil(year, 10, lastSunday(year, 10)) * 86400LL + 3600;
    } else {  // du 2e dimanche de mars au 1er de novembre, à 2 h, heure locale
      summer_from = daysFromCivil(year, 3, nthSunday(year, 3, 2)) * 86400LL + 2 * 3600 - m.utc * 60;
      summer_to = daysFromCivil(year, 11, nthSunday(year, 11, 1)) * 86400LL + 2 * 3600 - (m.utc + 60) * 60;
    }
    bool summer = m.dst != SUMMER_NONE && now >= summer_from && now < summer_to;
    time_t local = now + (m.utc + (summer ? 60 : 0)) * 60;
    gmtime_r(&local, &t);
    uint16_t minute = t.tm_hour * 60 + t.tm_min;
    return t.tm_wday >= 1 && t.tm_wday <= 5 && minute >= m.open && minute < m.close;
  }
  return -1;
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
  in = (uint8_t *)heap_caps_malloc(IN_SIZE, MALLOC_CAP_SPIRAM);
  // La partition est formatée au premier démarrage qui en a besoin
  if (LittleFS.begin(true)) {
    File k = LittleFS.open(KNOWN_PATH, "r");
    if (k) {
      size_t len = min(k.size() - k.size() % sizeof(Known), STOCKS_MAX * sizeof(Known));
      if (k.read((uint8_t *)known, len) == len) known_next = (len / sizeof(Known)) % STOCKS_MAX;
      k.close();
    }
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
  tls = new NetworkClientSecure();
  // Le certificat du serveur n'est pas vérifié : rien de secret n'est envoyé, et ce qui est reçu
  // n'est qu'affiché. Un tiers sur le chemin pourrait au pire fausser les cours à l'écran.
  tls->setInsecure();
  if (head && in) xTaskCreatePinnedToCoreWithCaps(stocksTask, "actions", 12288, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);
}

uint32_t stocksVersion() {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = current->count && valid && millis() - fetched_at < MAX_AGE_MS ? version : 0;
  xSemaphoreGive(lock);
  return v;
}

void stocksGet(Stocks &out) {
  // Les titres réels seulement, pas les 250 emplacements
  xSemaphoreTake(lock, portMAX_DELAY);
  uint8_t n = out.count = current->count;
  memcpy(out.stocks, current->stocks, n * sizeof(Stock));
  memcpy(out.title, current->title, n * sizeof(current->title[0]));
  memcpy(out.price, current->price, n * sizeof(double));
  memcpy(out.previous, current->previous, n * sizeof(double));
  xSemaphoreGive(lock);
}

int32_t stocksAge() {
  xSemaphoreTake(lock, portMAX_DELAY);
  int32_t age = valid ? (millis() - fetched_at) / 1000 : -1;
  xSemaphoreGive(lock);
  return age;
}

uint16_t stocksRoundSeconds() {
  return round_seconds;
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

// Enregistre les correspondances trouvées : celles qui ont un numéro, les inconnues seront recherchées
static void saveKnown() {
  static Known *copy = (Known *)heap_caps_malloc(STOCKS_MAX * sizeof(Known), MALLOC_CAP_SPIRAM);  // seule la boucle passe ici
  size_t n = 0;
  for (uint8_t i = 0; i < STOCKS_MAX; i++) {
    if (known[i].id) copy[n++] = known[i];
  }
  File f = LittleFS.open(KNOWN_PATH, "w");
  if (f) f.write((const uint8_t *)copy, n * sizeof(Known)), f.close();
}

void stocksLoop(bool dark) {
  static uint32_t waiting_since;
  if (!dirty && !known_dirty) {
    waiting_since = millis();
    return;
  }
  if (!dark && millis() - waiting_since < 10UL * 60 * 1000) return;
  waiting_since = millis();
  if (known_dirty) {
    known_dirty = false;
    saveKnown();
  }
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
