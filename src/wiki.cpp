#include "wiki.h"
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <freertos/idf_additions.h>
#include "jpeg.h"
#include "net.h"
#include "text.h"

#define HOST             "fr.wikipedia.org"
#define USER_AGENT       "ecran-esp32/1.0 (https://github.com/iooner/Waveshare-ESP32-S3-5B-PIO)"  // exigé par Wikimedia
#define HTTP_TIMEOUT_MS  10000
#define FIRST_READ_MS    40000                 // après les autres lectures HTTPS du démarrage
#define CHECK_MS         (60UL * 60 * 1000)    // la rubrique a-t-elle changé ? Elle change une fois par jour.
#define RETRY_MS         (10UL * 60 * 1000)    // après un échec
#define MAX_AGE_MS       (36UL * 60 * 60 * 1000)
#define RESP_SIZE        (128 * 1024)          // la plus grosse réponse est l'image, de 5 à 60 Ko
// Le texte de la rubrique du jour, mis à jour chaque jour par un robot de Wikipédia
#define RUBRIC_PATH      "/w/index.php?title=Wikip%C3%A9dia:Accueil_principal/Lumi%C3%A8re_sur_(copie_sans_mod%C3%A8les)&action=raw"

static WikiArticle current;
static uint16_t *images[2];  // l'image affichée et celle du prochain article, en PSRAM
static uint8_t next_image;
static uint32_t version = 0, fetched_at;
static bool valid = false;
static SemaphoreHandle_t lock;
static char *resp;  // réponse en cours, en PSRAM
static size_t resp_len;

// GET https://host/path. Renvoie le corps de la réponse, ou nul si le serveur ne répond pas par 200.
static const char *get(const char *host, const char *path) {
  NetHttps one_at_a_time;
  NetworkClientSecure tls;
  // Le certificat n'est pas vérifié : rien de secret n'est envoyé, et ce qui est reçu n'est qu'affiché
  tls.setInsecure();
  if (!tls.connect(host, 443, HTTP_TIMEOUT_MS)) return nullptr;
  // HTTP/1.0 : la réponse arrive d'un bloc, puis le serveur ferme la connexion
  tls.printf("GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n\r\n", path, host);
  size_t len = 0;
  uint32_t t0 = millis();
  while (len < RESP_SIZE - 1 && millis() - t0 < HTTP_TIMEOUT_MS && (tls.connected() || tls.available())) {
    int got = tls.read((uint8_t *)resp + len, RESP_SIZE - 1 - len);
    if (got > 0) len += got;
    else delay(5);
  }
  tls.stop();
  resp[len] = 0;
  char *body = strstr(resp, "\r\n\r\n");
  if (!body || strncmp(resp, "HTTP/1.", 7) != 0 || strncmp(resp + 8, " 200", 4) != 0) return nullptr;
  resp_len = len - (body + 4 - resp);
  return body + 4;
}

// Encode pour une adresse ce qui n'est ni lettre, ni chiffre, ni - _ . ~
static void urlEncode(const char *in, char *out, size_t cap) {
  size_t n = 0;
  for (const uint8_t *p = (const uint8_t *)in; *p && n + 4 < cap; p++) {
    if (isalnum(*p) || strchr("-_.~", *p)) out[n++] = *p;
    else n += snprintf(out + n, cap - n, "%%%02X", *p);
  }
  out[n] = 0;
}

// Copie une chaîne JSON qui commence à `from` (après son guillemet), échappements compris
static void jsonString(const char *from, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = from; *p && *p != '"' && n + 4 < cap; p++) {
    if (*p != '\\') {
      out[n++] = *p;
      continue;
    }
    char e = *++p;
    if (e == 'u' && isxdigit((uint8_t)p[1])) {
      char hex[5] = {p[1], p[2], p[3], p[4], 0};
      uint32_t c = strtoul(hex, nullptr, 16);
      p += 4;
      if (c < 0x80) out[n++] = c;
      else if (c < 0x800) out[n++] = 0xC0 | c >> 6, out[n++] = 0x80 | (c & 0x3F);
      else if (c < 0xD800 || c > 0xDFFF) out[n++] = 0xE0 | c >> 12, out[n++] = 0x80 | (c >> 6 & 0x3F), out[n++] = 0x80 | (c & 0x3F);
    } else if (e == 'n' || e == 't' || e == 'r') {
      out[n++] = ' ';
    } else if (e) {
      out[n++] = e;
    } else {
      break;
    }
  }
  out[n] = 0;
}

// Cible d'un lien [[cible|texte]] qui commence à `from` (après les crochets)
static void linkTarget(const char *from, char *out, size_t cap) {
  size_t n = strcspn(from, "|]#\n");
  n = min(n, cap - 1);
  memcpy(out, from, n);
  out[n] = 0;
}

static bool isFile(const char *link) {
  return strncasecmp(link, "Fichier:", 8) == 0 || strncasecmp(link, "File:", 5) == 0 || strncasecmp(link, "Image:", 6) == 0;
}

// Lit la rubrique du jour, puis son article et son image. Faux si un serveur ne répond pas.
static bool fetch() {
  // La rubrique, en wikicode : [[Fichier:Image.jpg|right|150px|...]] puis L'''[[Article]]''' est...
  const char *rubric = get(HOST, RUBRIC_PATH);
  if (!rubric) return false;
  static char title[128], file[128];
  title[0] = file[0] = 0;
  for (const char *p = strstr(rubric, "[["); p && (!title[0] || !file[0]); p = strstr(p + 2, "[[")) {
    if (isFile(p + 2)) {
      if (!file[0]) linkTarget(strchr(p, ':') + 1, file, sizeof(file));
    } else if (!title[0] && !strchr("#:", p[2]) && strncasecmp(p + 2, "Catégorie:", 11) != 0) {
      linkTarget(p + 2, title, sizeof(title));
    }
  }
  if (!title[0]) return false;
  // Même article que la dernière fois : rien d'autre à lire
  xSemaphoreTake(lock, portMAX_DELAY);
  bool same = valid && strcmp(title, current.title) == 0;
  if (same) fetched_at = millis();
  xSemaphoreGive(lock);
  if (same) return true;

  // Le début de l'introduction de l'article et l'adresse d'une vignette de l'image, en une requête
  static char path[768], encoded[400];
  urlEncode(title, encoded, sizeof(encoded));
  int n = snprintf(path, sizeof(path),
                   "/w/api.php?action=query&format=json&formatversion=2&redirects=1&prop=extracts%%7Cimageinfo&exintro=1"
                   "&explaintext=1&exchars=700&iiprop=url%%7Cmime&iiurlwidth=%u&iiurlheight=%u&titles=%s",
                   WIKI_IMAGE_MAX, WIKI_IMAGE_MAX, encoded);
  if (file[0]) {
    urlEncode(file, encoded, sizeof(encoded));
    snprintf(path + n, sizeof(path) - n, "%%7CFichier:%s", encoded);
  }
  const char *json = get(HOST, path);
  if (!json) return false;
  static WikiArticle a;
  a = {};
  strlcpy(a.title, title, sizeof(a.title));
  const char *extract = strstr(json, "\"extract\":\"");
  if (extract) jsonString(extract + 11, a.text, sizeof(a.text));
  char thumb[512] = "";
  const char *url = strstr(json, "\"thumburl\":\""), *mime = strstr(json, "\"mime\":\"image/jpeg\"");
  if (url && mime) jsonString(url + 12, thumb, sizeof(thumb));
  for (char *t : {a.title, a.text}) {
    toLatin1(t);
    dropUnsupported(t);
    // Une seule espace entre deux mots : le texte de Wikipédia en a parfois deux
    char *w = t;
    for (const char *r = t; *r; r++) {
      if (*r != ' ' || (w > t && w[-1] != ' ')) *w++ = *r;
    }
    *w = 0;
  }

  // La vignette : https://thumb.wikimedia.org/wikipedia/commons/thumb/... Le décodeur ne lit que
  // les JPEG ; sans image, la page s'en passe.
  if (strncmp(thumb, "https://", 8) == 0 && images[next_image]) {
    char host[64];
    const char *slash = strchr(thumb + 8, '/');
    if (slash && slash - thumb - 8 < (int)sizeof(host)) {
      strlcpy(host, thumb + 8, slash - thumb - 8 + 1);
      const char *body = get(host, slash);
      if (body && jpegDecode((const uint8_t *)body, resp_len, images[next_image], WIKI_IMAGE_MAX, WIKI_IMAGE_MAX,
                             a.image_w, a.image_h)) {
        a.image = images[next_image];
      }
    }
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  current = a;
  valid = true;
  version++;
  fetched_at = millis();
  xSemaphoreGive(lock);
  if (a.image) next_image ^= 1;
  Serial.printf("Wikipédia : %s, %s\n", a.title, a.image ? "avec image" : "sans image");
  return true;
}

static void wikiTask(void *) {
  delay(FIRST_READ_MS);
  for (;;) {
    uint32_t wait = 500;  // en attendant le Wi-Fi
    if (WiFi.status() == WL_CONNECTED) {
      bool ok = fetch();
      if (!ok) Serial.println("Wikipédia : pas de réponse");
      wait = ok ? CHECK_MS : RETRY_MS;
    }
    delay(wait);
  }
}

void wikiBegin() {
  lock = xSemaphoreCreateMutex();
  resp = (char *)heap_caps_malloc(RESP_SIZE, MALLOC_CAP_SPIRAM);
  for (uint16_t *&b : images) b = (uint16_t *)heap_caps_malloc(WIKI_IMAGE_MAX * WIKI_IMAGE_MAX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  // Pile en PSRAM : permis parce que la tâche n'écrit jamais en flash (voir weather.cpp)
  if (resp) xTaskCreatePinnedToCoreWithCaps(wikiTask, "wiki", 12288, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
}

uint32_t wikiVersion() {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t v = valid && millis() - fetched_at < MAX_AGE_MS ? version : 0;
  xSemaphoreGive(lock);
  return v;
}

void wikiGet(WikiArticle &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = current;
  xSemaphoreGive(lock);
}

int32_t wikiAge() {
  xSemaphoreTake(lock, portMAX_DELAY);
  int32_t age = valid ? (millis() - fetched_at) / 1000 : -1;
  xSemaphoreGive(lock);
  return age;
}
