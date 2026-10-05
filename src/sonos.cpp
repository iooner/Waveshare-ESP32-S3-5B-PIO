#include "sonos.h"
#include "text.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <freertos/idf_additions.h>
#include <esp32s3/rom/tjpgd.h>

#define SONOS_PORT       1400
#define MAX_SPEAKERS     8
#define HTTP_TIMEOUT_MS  1500
#define POLL_MS          2000                 // quand une enceinte joue
#define IDLE_POLL_MS     5000                 // quand rien ne joue : il faut interroger chaque enceinte
#define DISCOVERY_MS     (10UL * 60 * 1000)   // les enceintes changent rarement d'adresse

struct Speaker {
  IPAddress ip;
  char room[32];
};

static Speaker speakers[MAX_SPEAKERS];
static uint8_t speaker_count = 0;
static uint8_t active = 0;  // enceinte interrogée en premier : la dernière qui jouait
static SonosTrack current = {};
static SemaphoreHandle_t lock;
#define RESP_SIZE  4096
static char *resp;  // réponse HTTP en cours, en PSRAM ; GetPositionInfo fait ~1,5 Ko

// Pochette : deux images en PSRAM, celle qui est publiée et celle en cours de décodage
#define ART_PX        (SONOS_ART_SIZE * SONOS_ART_SIZE)
#define ART_JPEG_MAX  (96 * 1024)  // une pochette de 300 x 300 pèse ~30 Ko
#define ART_POOL      4096         // mémoire de travail du décodeur JPEG
static uint16_t *art_buf[2];
static uint8_t *art_jpeg, *art_pool;
static const uint16_t *art = nullptr;
static uint32_t art_version = 0;
static uint8_t art_next = 0;
static char art_url[160], art_shown[160];

// --- Texte ---

// Copie dans out ce qui se trouve entre deux balises. Faux si elles sont absentes.
static bool between(const char *text, const char *open, const char *close, char *out, size_t cap) {
  const char *a = strstr(text, open);
  if (!a) return false;
  a += strlen(open);
  const char *b = strstr(a, close);
  if (!b) return false;
  size_t n = min((size_t)(b - a), cap - 1);
  memcpy(out, a, n);
  out[n] = 0;
  return true;
}

// Ecrit un point de code en UTF-8, renvoie la suite de la chaîne
static char *putUtf8(char *w, uint32_t cp) {
  if (cp < 0x80) {
    *w++ = cp;
  } else if (cp < 0x800) {
    *w++ = 0xC0 | cp >> 6;
    *w++ = 0x80 | (cp & 0x3F);
  } else if (cp < 0x10000) {
    *w++ = 0xE0 | cp >> 12;
    *w++ = 0x80 | (cp >> 6 & 0x3F);
    *w++ = 0x80 | (cp & 0x3F);
  } else {
    *w++ = '?';
  }
  return w;
}

// Remplace sur place les entités XML (&lt; &amp; &#233; ...). Le résultat n'est jamais plus long.
static void unescape(char *s) {
  static const struct {
    const char *name;
    char c;
  } NAMED[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
  char *w = s;
  for (char *r = s; *r;) {
    if (*r == '&') {
      bool done = false;
      for (const auto &e : NAMED) {
        size_t n = strlen(e.name);
        if (strncmp(r, e.name, n) == 0) {
          *w++ = e.c;
          r += n;
          done = true;
          break;
        }
      }
      if (done) continue;
      if (r[1] == '#') {
        char *end;
        uint32_t cp = r[2] == 'x' ? strtoul(r + 3, &end, 16) : strtoul(r + 2, &end, 10);
        if (*end == ';' && cp) {
          w = putUtf8(w, cp);
          r = end + 1;
          continue;
        }
      }
    }
    *w++ = *r++;
  }
  *w = 0;
}

// "H:MM:SS" en secondes ; 0 pour "NOT_IMPLEMENTED"
static uint16_t parseTime(const char *s) {
  unsigned h, m, sec;
  return sscanf(s, "%u:%u:%u", &h, &m, &sec) == 3 ? h * 3600 + m * 60 + sec : 0;
}

// --- Réseau ---

// Lit la réponse dans resp jusqu'à la fermeture de la connexion. Renvoie sa longueur.
static size_t readResponse(WiFiClient &c) {
  size_t n = 0;
  uint32_t t0 = millis();
  while (n < RESP_SIZE - 1 && millis() - t0 < HTTP_TIMEOUT_MS && (c.connected() || c.available())) {
    int got = c.read((uint8_t *)resp + n, RESP_SIZE - 1 - n);
    if (got > 0) n += got;
    else delay(2);
  }
  resp[n] = 0;
  c.stop();
  return n;
}

// Appelle une action UPnP de l'enceinte. La réponse brute est dans resp.
static bool soap(IPAddress ip, const char *path, const char *service, const char *action, const char *args) {
  WiFiClient c;
  if (!c.connect(ip, SONOS_PORT, HTTP_TIMEOUT_MS)) return false;
  char body[320];
  int len = snprintf(body, sizeof(body),
                     "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                     "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                     "<u:%s xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">%s</u:%s></s:Body></s:Envelope>",
                     action, service, args, action);
  c.printf("POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
           "SOAPACTION: \"urn:schemas-upnp-org:service:%s:1#%s\"\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s",
           path, ip.toString().c_str(), SONOS_PORT, service, action, len, body);
  return readResponse(c) > 0 && strstr(resp, " 200 ") != nullptr;
}

static bool transport(IPAddress ip, const char *action) {
  return soap(ip, "/MediaRenderer/AVTransport/Control", "AVTransport", action, "<InstanceID>0</InstanceID>");
}

// Nom de la pièce où se trouve l'enceinte
static void readRoom(Speaker &sp) {
  if (!soap(sp.ip, "/DeviceProperties/Control", "DeviceProperties", "GetZoneAttributes", "") ||
      !between(resp, "<CurrentZoneName>", "</CurrentZoneName>", sp.room, sizeof(sp.room))) {
    strlcpy(sp.room, "Sonos", sizeof(sp.room));
    return;
  }
  unescape(sp.room);
  toLatin1(sp.room);
}

// Recherche des enceintes par SSDP : elles répondent directement à notre adresse
static void discover() {
  static const char MSEARCH[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                "MX: 1\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
  WiFiUDP udp;
  if (!udp.begin(1901)) return;
  IPAddress found[MAX_SPEAKERS];
  uint8_t count = 0;
  for (uint8_t attempt = 0; attempt < 3 && count == 0; attempt++) {
    udp.beginPacket(IPAddress(239, 255, 255, 250), 1900);
    udp.write((const uint8_t *)MSEARCH, sizeof(MSEARCH) - 1);
    udp.endPacket();
    uint32_t t0 = millis();
    while (millis() - t0 < 1500) {
      if (!udp.parsePacket()) {
        delay(20);
        continue;
      }
      int n = udp.read((uint8_t *)resp, RESP_SIZE - 1);
      resp[max(n, 0)] = 0;
      IPAddress ip = udp.remoteIP();
      bool known = !strstr(resp, "ZonePlayer");  // autre appareil UPnP : ignoré
      for (uint8_t i = 0; i < count; i++) known |= found[i] == ip;
      if (!known && count < MAX_SPEAKERS) found[count++] = ip;
    }
  }
  udp.stop();
  if (count == 0) return;  // on garde la liste précédente

  speaker_count = count;
  active = 0;
  for (uint8_t i = 0; i < count; i++) {
    speakers[i].ip = found[i];
    readRoom(speakers[i]);
    Serial.printf("Sonos : %s (%s)\n", speakers[i].room, found[i].toString().c_str());
  }
}

// Etat et morceau d'une enceinte. Faux si elle n'a rien à montrer : arrêtée, injoignable, ou
// simple membre d'un groupe (c'est alors l'enceinte principale du groupe qui a les informations).
static bool readSpeaker(const Speaker &sp, SonosTrack &t) {
  char buf[32];
  if (!transport(sp.ip, "GetTransportInfo")) return false;
  if (!between(resp, "<CurrentTransportState>", "</CurrentTransportState>", buf, sizeof(buf))) return false;
  SonosState state = strcmp(buf, "PLAYING") == 0 || strcmp(buf, "TRANSITIONING") == 0 ? SONOS_PLAYING
                     : strcmp(buf, "PAUSED_PLAYBACK") == 0                              ? SONOS_PAUSED
                                                                                        : SONOS_NONE;
  if (state == SONOS_NONE) return false;

  if (!transport(sp.ip, "GetPositionInfo")) return false;
  t = {};
  // Les métadonnées sont un document XML rangé dans un champ XML : ses balises arrivent
  // échappées, et son texte échappé deux fois
  if (!between(resp, "&lt;dc:title&gt;", "&lt;/dc:title&gt;", t.title, sizeof(t.title))) return false;
  between(resp, "&lt;dc:creator&gt;", "&lt;/dc:creator&gt;", t.artist, sizeof(t.artist));
  between(resp, "&lt;upnp:album&gt;", "&lt;/upnp:album&gt;", t.album, sizeof(t.album));
  art_url[0] = 0;
  between(resp, "&lt;upnp:albumArtURI&gt;", "&lt;/upnp:albumArtURI&gt;", art_url, sizeof(art_url));
  unescape(art_url);
  unescape(art_url);
  for (char *s : {t.title, t.artist, t.album}) {
    unescape(s);
    unescape(s);
    toLatin1(s);
  }
  if (between(resp, "<TrackDuration>", "</TrackDuration>", buf, sizeof(buf))) t.duration = parseTime(buf);
  if (between(resp, "<RelTime>", "</RelTime>", buf, sizeof(buf))) t.position = parseTime(buf);
  t.position_at = millis();
  t.state = state;
  strlcpy(t.room, sp.room, sizeof(t.room));
  return true;
}

// --- Pochette ---

struct ArtIo {
  const uint8_t *data;
  size_t len, pos;
  uint16_t *out;
};

static UINT artRead(JDEC *jd, BYTE *buf, UINT n) {
  ArtIo *io = (ArtIo *)jd->device;
  n = min((size_t)n, io->len - io->pos);
  if (buf) memcpy(buf, io->data + io->pos, n);
  io->pos += n;
  return n;
}

// Reçoit un bloc décodé en RGB888 et le range en RGB565, coupé à la taille de la pochette
static UINT artWrite(JDEC *jd, void *bitmap, JRECT *r) {
  ArtIo *io = (ArtIo *)jd->device;
  const uint8_t *px = (const uint8_t *)bitmap;
  for (int y = r->top; y <= r->bottom; y++) {
    for (int x = r->left; x <= r->right; x++, px += 3) {
      if (x < SONOS_ART_SIZE && y < SONOS_ART_SIZE) {
        io->out[y * SONOS_ART_SIZE + x] = (px[0] & 0xF8) << 8 | (px[1] & 0xFC) << 3 | px[2] >> 3;
      }
    }
  }
  return 1;
}

// Télécharge et décode la pochette dans l'image de réserve. Spotify sert ses pochettes en HTTP
// simple, et en 300 x 300 quand on change le préfixe de leur identifiant.
static bool fetchArt(const char *url) {
  static const char BASE[] = "https://i.scdn.co/image/", BIG[] = "ab67616d0000b273", SMALL[] = "ab67616d00001e02";
  if (strncmp(url, BASE, sizeof(BASE) - 1) != 0 || !art_jpeg) return false;
  char id[80];
  strlcpy(id, url + sizeof(BASE) - 1, sizeof(id));
  if (strncmp(id, BIG, sizeof(BIG) - 1) == 0) memcpy(id, SMALL, sizeof(SMALL) - 1);

  WiFiClient c;
  if (!c.connect("i.scdn.co", 80, 3000)) return false;
  c.printf("GET /image/%s HTTP/1.1\r\nHost: i.scdn.co\r\nConnection: close\r\n\r\n", id);
  size_t n = 0;
  uint32_t t0 = millis();
  while (n < ART_JPEG_MAX && millis() - t0 < 5000 && (c.connected() || c.available())) {
    int got = c.read(art_jpeg + n, ART_JPEG_MAX - n);
    if (got > 0) n += got;
    else delay(2);
  }
  c.stop();

  // Le JPEG commence après la ligne vide qui termine les en-têtes
  const uint8_t *body = (const uint8_t *)memmem(art_jpeg, n, "\r\n\r\n", 4);
  if (!body || memcmp(art_jpeg + 9, "200", 3) != 0) return false;
  body += 4;

  uint16_t *out = art_buf[art_next];
  memset(out, 0, ART_PX * sizeof(uint16_t));
  ArtIo io = {body, n - (size_t)(body - art_jpeg), 0, out};
  JDEC jd;
  if (jd_prepare(&jd, artRead, art_pool, ART_POOL, &io) != JDR_OK) return false;
  // Décodeur de la ROM : réductions de 1/2, 1/4, 1/8 seulement
  uint8_t scale = 0;
  while (scale < 3 && (jd.width >> scale) > SONOS_ART_SIZE) scale++;
  return jd_decomp(&jd, artWrite, scale) == JDR_OK;
}

// Met la pochette à jour quand le morceau en change. L'ancienne reste publiée pendant le chargement.
static void updateArt(bool playing) {
  if (!playing) art_url[0] = 0;
  if (strcmp(art_url, art_shown) == 0) return;
  strlcpy(art_shown, art_url, sizeof(art_shown));
  bool ok = art_url[0] && fetchArt(art_url);
  if (art_url[0]) Serial.printf("Sonos : pochette %s\n", ok ? "chargée" : "indisponible");
  xSemaphoreTake(lock, portMAX_DELAY);
  art = ok ? art_buf[art_next] : nullptr;
  art_version++;
  xSemaphoreGive(lock);
  if (ok) art_next ^= 1;
}

static void sonosTask(void *) {
  uint32_t last_discovery = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      delay(500);
      continue;
    }
    if (speaker_count == 0 || millis() - last_discovery > DISCOVERY_MS) {
      discover();
      last_discovery = millis();
    }

    // L'enceinte qui jouait d'abord : tant qu'elle joue, on n'interroge qu'elle
    SonosTrack track = {}, t;
    for (uint8_t k = 0; k < speaker_count; k++) {
      uint8_t i = (active + k) % speaker_count;
      if (!readSpeaker(speakers[i], t)) continue;
      if (t.state == SONOS_PLAYING) {
        track = t;
        active = i;
        break;
      }
      if (track.state == SONOS_NONE) track = t;  // première enceinte en pause
    }

    // Une lecture manquée (réseau, changement de morceau) ne doit pas faire basculer l'affichage
    static uint8_t misses = 0;
    if (track.state != SONOS_PLAYING && current.state == SONOS_PLAYING && ++misses < 2) {
      delay(POLL_MS);
      continue;
    }
    misses = 0;

    if (track.state != current.state || strcmp(track.title, current.title) != 0) {
      if (track.state == SONOS_NONE) Serial.println("Sonos : rien en lecture");
      else Serial.printf("Sonos : %s%s, %s - %s\n", track.room, track.state == SONOS_PAUSED ? " (pause)" : "", track.artist, track.title);
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    current = track;
    xSemaphoreGive(lock);
    updateArt(track.state == SONOS_PLAYING);
    delay(track.state == SONOS_PLAYING ? POLL_MS : IDLE_POLL_MS);
  }
}

void sonosBegin() {
  lock = xSemaphoreCreateMutex();
  for (uint16_t *&b : art_buf) b = (uint16_t *)heap_caps_malloc(ART_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  art_jpeg = (uint8_t *)heap_caps_malloc(ART_JPEG_MAX, MALLOC_CAP_SPIRAM);
  art_pool = (uint8_t *)heap_caps_malloc(ART_POOL, MALLOC_CAP_SPIRAM);
  if (!art_buf[0] || !art_buf[1] || !art_pool) art_jpeg = nullptr;  // pas de pochette
  resp = (char *)heap_caps_malloc(RESP_SIZE, MALLOC_CAP_SPIRAM);
  // Pile en PSRAM, pour garder la RAM interne à l'écran : permis parce que la tâche n'écrit jamais en flash
  if (resp) xTaskCreatePinnedToCoreWithCaps(sonosTask, "sonos", 8192, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM);
}

void sonosGet(SonosTrack &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  out = current;
  xSemaphoreGive(lock);
}

const uint16_t *sonosArt(uint32_t &version) {
  xSemaphoreTake(lock, portMAX_DELAY);
  const uint16_t *a = art;
  version = art_version;
  xSemaphoreGive(lock);
  return a;
}
