#include "agenda.h"
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <freertos/idf_additions.h>
#include "net.h"
#include "text.h"

#define LGHS_URL         "https://lghs.be/calendar.php"
#define FEEDS            (1 + AGENDA_URLS)        // celui du hackerspace, puis les agendas personnels
#define URL_SIZE         320
#define HTTP_TIMEOUT_MS  15000
#define FIRST_READ_MS    40000                    // pas de lecture juste après le démarrage : comme crypto.cpp
#define REFRESH_MS       (60UL * 60 * 1000)
#define RETRY_MS         (10UL * 60 * 1000)       // après un échec
#define MAX_AGE_MS       (48UL * 60 * 60 * 1000)  // au-delà, le calendrier a pu changer : mieux vaut ne rien afficher
#define UPDATE_MS        60000                    // les listes dépendent de l'heure : un événement fini en sort
#define CHUNK_SIZE       4096

// Un calendrier suivi. Tout est en PSRAM.
struct Feed {
  char url[URL_SIZE];   // vide : emplacement libre
  IcalCalendar *cal;    // dernier calendrier lu en entier, nul tant qu'il n'y en a pas
  uint32_t fetched_at;  // de cette lecture
  uint32_t next_fetch;
  bool due;             // à lire dès que possible
  char problem[64];     // pourquoi la dernière lecture a échoué ; vide si elle a réussi
  char name[AGENDA_NAME_SIZE];  // nom donné dans le back office
};

static Feed *feeds;
static IcalCalendar *scratch;  // calendrier en cours de lecture : il ne remplace l'ancien qu'une fois complet
static char *chunk;            // morceau de réponse HTTP en cours
static Agenda *lists;           // les listes publiées, en PSRAM
static uint32_t versions[AGENDA_LISTS];
static SemaphoreHandle_t lock;  // protège les adresses et les listes publiées
static TaskHandle_t task;

static AgendaList listOf(uint8_t feed) {
  return feed == 0 ? AGENDA_LGHS : AGENDA_MINE;
}

// Découpe une adresse. Faux si elle n'est pas de la forme attendue.
static bool splitUrl(const char *url, bool &secure, char *host, size_t host_cap, const char *&path) {
  const char *rest;
  if (strncmp(url, "https://", 8) == 0) secure = true, rest = url + 8;
  else if (strncmp(url, "webcal://", 9) == 0) secure = true, rest = url + 9;  // lien d'abonnement : le même fichier, en HTTPS
  else if (strncmp(url, "http://", 7) == 0) secure = false, rest = url + 7;
  else return false;
  size_t n = strcspn(rest, "/");
  if (n == 0 || n >= host_cap) return false;
  memcpy(host, rest, n);
  host[n] = 0;
  path = rest[n] ? rest + n : "/";
  return true;
}

// Lit un calendrier au fil de l'eau dans `scratch`. Faux s'il n'arrive pas en entier : l'ancien
// est alors gardé, et `problem` dit pourquoi.
static bool fetch(const char *url, char *problem, size_t cap) {
  bool secure;
  char host[96];
  const char *path;
  problem[0] = 0;
  if (!splitUrl(url, secure, host, sizeof(host), path)) {
    strlcpy(problem, "adresse mal formée", cap);
    return false;
  }

  NetHttps one_at_a_time;
  NetworkClientSecure tls;
  NetworkClient plain;
  NetworkClient &c = secure ? (NetworkClient &)tls : plain;
  // Certificat non vérifié : la carte ne peut pas authentifier le serveur. Avec une adresse
  // secrète, un tiers placé sur le chemin pourrait donc la lire.
  tls.setInsecure();
  if (!c.connect(host, secure ? 443 : 80, HTTP_TIMEOUT_MS)) {
    snprintf(problem, cap, "connexion impossible à %s", host);
    return false;
  }
  // HTTP/1.0 : la réponse arrive d'un bloc, puis le serveur ferme la connexion
  c.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ecran-esp32\r\n\r\n", path, host);

  icalBegin(*scratch, time(nullptr));
  // En-têtes : seule leur première ligne compte (« HTTP/1.0 200 OK »). Le reste est passé sans être
  // gardé, jusqu'à la ligne vide qui les termine : ceux de Google dépassent 4 Ko.
  char first[16];
  size_t first_len = 0, total = 0;
  uint8_t newlines = 0;
  bool in_body = false, ok = true;
  uint32_t last = millis();
  while (ok && millis() - last < HTTP_TIMEOUT_MS && (c.connected() || c.available())) {
    int got = c.read((uint8_t *)chunk, CHUNK_SIZE);
    if (got <= 0) {
      delay(5);
      continue;
    }
    last = millis();
    total += got;
    const char *p = chunk, *end = chunk + got;
    for (; p < end && !in_body; p++) {
      if (first_len < sizeof(first) - 1) first[first_len++] = *p;
      if (*p == '\n') in_body = ++newlines == 2;
      else if (*p != '\r') newlines = 0;
      if (!in_body) continue;
      first[first_len] = 0;
      const char *status = strchr(first, ' ');
      ok = strncmp(first, "HTTP/", 5) == 0 && status && strncmp(status, " 200", 4) == 0;
      if (!ok) snprintf(problem, cap, "réponse %.4s de %s", status ? status + 1 : "?", host);
    }
    if (ok && in_body && p < end) icalFeed(*scratch, p, end - p);
  }
  c.stop();
  icalEnd(*scratch);
  Serial.printf("Agenda : %s, %u octets, %u événements qui se répètent, %u à venir%s\n", host, (unsigned)total,
                scratch->master_count, scratch->single_count, scratch->truncated ? ", liste pleine" : "");
  if (ok && !scratch->complete) snprintf(problem, cap, "calendrier incomplet, %u octets reçus", (unsigned)total);
  else if (ok && scratch->truncated) snprintf(problem, cap, "agenda trop fourni (%u qui se répètent, %u à venir)", scratch->master_count, scratch->single_count);
  return ok && scratch->complete;
}

// Refait les deux listes de prochains événements et publie celles qui ont changé
static void update() {
  if (!netTimeSynced()) return;
  time_t now = time(nullptr);
  Agenda fresh[AGENDA_LISTS] = {};
  for (uint8_t f = 0; f < FEEDS; f++) {
    Feed &feed = feeds[f];
    if (!feed.url[0] || !feed.cal || millis() - feed.fetched_at > MAX_AGE_MS) continue;
    // Les événements de ce calendrier, glissés à leur place dans la liste, qui reste triée
    IcalEvent events[AGENDA_MAX];
    uint8_t count = icalUpcoming(*feed.cal, now, events, AGENDA_MAX);
    Agenda &list = fresh[listOf(f)];
    for (uint8_t i = 0; i < count; i++) {
      uint8_t at = list.count;
      while (at > 0 && list.events[at - 1].start > events[i].start) at--;
      if (at >= AGENDA_MAX) continue;
      if (list.count < AGENDA_MAX) list.count++;
      memmove(&list.events[at + 1], &list.events[at], (list.count - at - 1) * sizeof(IcalEvent));
      memmove(&list.source[at + 1], &list.source[at], list.count - at - 1);
      list.events[at] = events[i];
      list.source[at] = f ? f - 1 : 0;
      // Titre au format des polices : sans emoji ni rien d'autre qu'elles ne savent pas dessiner
      char *title = list.events[at].title;
      toLatin1(title);
      dropUnsupported(title);
      if (!title[0]) strlcpy(title, "(sans titre)", sizeof(list.events[at].title));
    }
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  for (uint8_t l = 0; l < AGENDA_LISTS; l++) {
    if (memcmp(&fresh[l], &lists[l], sizeof(Agenda)) == 0) continue;
    lists[l] = fresh[l];
    versions[l]++;
  }
  xSemaphoreGive(lock);
}

static void agendaTask(void *) {
  delay(FIRST_READ_MS);
  for (;;) {
    for (uint8_t f = 0; f < FEEDS && WiFi.status() == WL_CONNECTED && netTimeSynced(); f++) {
      Feed &feed = feeds[f];
      char url[URL_SIZE];
      xSemaphoreTake(lock, portMAX_DELAY);
      strlcpy(url, feed.url, sizeof(url));
      bool due = url[0] && (feed.due || (int32_t)(millis() - feed.next_fetch) >= 0);
      feed.due = false;
      xSemaphoreGive(lock);
      if (!due) continue;

      char problem[sizeof(feed.problem)];
      bool ok = fetch(url, problem, sizeof(problem));
      xSemaphoreTake(lock, portMAX_DELAY);
      strlcpy(feed.problem, problem, sizeof(feed.problem));
      if (strcmp(url, feed.url) != 0) {
        feed.due = true;  // l'adresse a changé pendant la lecture : ce calendrier n'est plus le bon
      } else if (ok) {
        IcalCalendar *old = feed.cal;
        feed.cal = scratch;
        // Le calendrier remplacé sert de brouillon à la prochaine lecture ; le premier en demande un neuf
        scratch = old ? old : (IcalCalendar *)heap_caps_malloc(sizeof(IcalCalendar), MALLOC_CAP_SPIRAM);
        feed.fetched_at = millis();
      }
      feed.next_fetch = millis() + (ok ? REFRESH_MS : RETRY_MS);
      xSemaphoreGive(lock);
      if (!ok) Serial.println("Agenda : calendrier illisible");
      if (!scratch) vTaskSuspend(nullptr);  // plus de mémoire : on en reste là
    }
    update();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UPDATE_MS));  // un changement d'adresse réveille la tâche
  }
}

void agendaBegin() {
  if (lock) return;  // deux pages s'en servent
  lock = xSemaphoreCreateMutex();
  feeds = (Feed *)heap_caps_calloc(FEEDS, sizeof(Feed), MALLOC_CAP_SPIRAM);
  scratch = (IcalCalendar *)heap_caps_malloc(sizeof(IcalCalendar), MALLOC_CAP_SPIRAM);
  chunk = (char *)heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_SPIRAM);
  lists = (Agenda *)heap_caps_calloc(AGENDA_LISTS, sizeof(Agenda), MALLOC_CAP_SPIRAM);
  if (!feeds || !scratch || !chunk || !lists) return;

  strlcpy(feeds[0].url, LGHS_URL, URL_SIZE);
  Preferences prefs;
  prefs.begin("agenda");
  for (uint8_t i = 0; i < AGENDA_URLS; i++) {
    char key[4] = {'u', (char)('1' + i), 0};
    if (prefs.isKey(key)) prefs.getString(key, feeds[1 + i].url, URL_SIZE);
    key[0] = 'n';
    if (prefs.isKey(key)) prefs.getString(key, feeds[1 + i].name, AGENDA_NAME_SIZE);
  }
  prefs.end();
  // Pile en PSRAM : permis parce que la tâche n'écrit jamais en flash (voir weather.cpp)
  xTaskCreatePinnedToCoreWithCaps(agendaTask, "agenda", 16384, nullptr, 1, &task, 0, MALLOC_CAP_SPIRAM);
}

uint32_t agendaVersion(AgendaList which) {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = lists && lists[which].count ? versions[which] : 0;
  xSemaphoreGive(lock);
  return v;
}

void agendaGet(AgendaList which, Agenda &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (lists) out = lists[which];
  else out.count = 0;
  xSemaphoreGive(lock);
}

int32_t agendaAge(AgendaList which) {
  int32_t age = -1;
  for (uint8_t f = 0; feeds && f < FEEDS; f++) {
    if (listOf(f) != which || !feeds[f].url[0] || !feeds[f].cal) continue;
    int32_t a = (millis() - feeds[f].fetched_at) / 1000;
    if (age < 0 || a < age) age = a;
  }
  return age;
}

const char *agendaProblem(AgendaList which) {
  for (uint8_t f = 0; feeds && f < FEEDS; f++) {
    if (listOf(f) == which && feeds[f].url[0] && feeds[f].problem[0]) return feeds[f].problem;
  }
  return "";
}

void agendaName(uint8_t index, char *out, size_t cap) {
  out[0] = 0;
  if (!feeds || index >= AGENDA_URLS) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  strlcpy(out, feeds[1 + index].name, cap);
  xSemaphoreGive(lock);
}

void agendaSetName(uint8_t index, const char *name) {
  if (!feeds || index >= AGENDA_URLS) return;
  Feed &feed = feeds[1 + index];
  xSemaphoreTake(lock, portMAX_DELAY);
  bool changed = strncmp(feed.name, name, AGENDA_NAME_SIZE - 1) != 0;
  if (changed) {
    strlcpy(feed.name, name, AGENDA_NAME_SIZE);
    versions[AGENDA_MINE]++;  // la page le montre : à redessiner
  }
  xSemaphoreGive(lock);
  if (!changed) return;
  Preferences prefs;
  prefs.begin("agenda");
  char key[4] = {'n', (char)('1' + index), 0};
  if (name[0]) prefs.putString(key, feed.name);
  else prefs.remove(key);
  prefs.end();
}

bool agendaHasUrl(uint8_t index) {
  return feeds && index < AGENDA_URLS && feeds[1 + index].url[0];
}

void agendaSetUrl(uint8_t index, const char *url) {
  if (!feeds || index >= AGENDA_URLS || strlen(url) >= URL_SIZE) return;
  Feed &feed = feeds[1 + index];
  xSemaphoreTake(lock, portMAX_DELAY);
  bool changed = strcmp(feed.url, url) != 0;
  if (changed) {
    strlcpy(feed.url, url, URL_SIZE);
    feed.fetched_at = millis() - MAX_AGE_MS - 1;  // l'ancien calendrier ne vaut plus pour cette adresse
    feed.problem[0] = 0;
    feed.due = true;
  }
  xSemaphoreGive(lock);
  if (!changed) return;
  Preferences prefs;
  prefs.begin("agenda");
  char key[4] = {'u', (char)('1' + index), 0};
  if (url[0]) prefs.putString(key, url);
  else prefs.remove(key);
  prefs.end();
  if (task) xTaskNotifyGive(task);
}
