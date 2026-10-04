#include "web.h"
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include "brightness.h"
#include "lcd.h"
#include "plugin.h"
#include "weather.h"

#define BUF_SIZE           4096  // une requête de navigateur fait moins de 1 Ko, la page ~3 Ko
#define HTTP_TIMEOUT_MS    1000
#define UPDATE_TIMEOUT_MS  10000  // silence maximal pendant l'envoi d'un firmware

static char *req, *page;  // requête reçue et page envoyée, en PSRAM
static size_t page_len;
static volatile bool updating = false;

static const char HEAD[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Écran</title><style>body{font:18px system-ui;max-width:24em;margin:2em auto;padding:0 1em;"
    "background:#111;color:#eee}label{display:block;margin:.7em 0}input[type=range]{width:14em;vertical-align:middle}"
    "input:not([type=checkbox]),button{font:inherit;padding:.3em .6em;margin:.2em .3em .2em 0}"
    "small{color:#999}</style><h1>Écran</h1><form action=/set><h2>Pages</h2>";

// Arguments : météo cochée ou non, luminosité de jour (deux fois), cycle coché ou non,
// luminosité de nuit (deux fois), latitude, longitude
static const char SETTINGS[] =
    "<label><input type=checkbox name=meteo%s> Météo</label><small>L'horloge reste toujours active.</small>"
    // Lâcher un curseur enregistre tout de suite, pour voir le résultat sur l'écran
    "<h2>Luminosité</h2><label><input type=range name=jour min=1 max=100 value=%u onchange=form.submit() "
    "oninput=nextElementSibling.textContent=value> <span>%u</span> %%</label>"
    "<label><input type=checkbox name=auto%s> Cycle automatique : baisse quand le soleil se couche</label>"
    "<label>La nuit <input type=range name=nuit min=0 max=100 value=%u onchange=form.submit() "
    "oninput=nextElementSibling.textContent=value> <span>%u</span> %%</label>"
    "<h2>Lieu de la météo</h2><input id=q placeholder='Chercher une ville'> <button type=button onclick=s()>"
    "Chercher</button><div id=r></div><label>Latitude <input id=lat name=lat value=%.4f></label>"
    "<label>Longitude <input id=lon name=lon value=%.4f></label><p><button>Enregistrer</button></form>"
    "<h2>Mise à jour</h2><input type=file id=f accept=.bin> <button type=button onclick=u()>Envoyer</button> "
    "<span id=m></span>";

// Arguments : heures et minutes de fonctionnement, signal Wi-Fi, RAM interne libre et son minimum,
// images ratées, luminosité, météo, partition, marge des piles web, météo et Sonos
static const char STATE[] =
    "<h2>État</h2><p>Allumé depuis %lu h %02lu min<br>Wi-Fi : %d dBm<br>RAM interne libre : %u Ko (au plus bas %u Ko)"
    "<br>Images ratées depuis le démarrage : %lu<br>Luminosité : %u %%<br>Météo : %s<br>Firmware dans la partition %s"
    "<br>Marge des piles : web %u, météo %u, Sonos %u octets";

static const char SCRIPT[] =
    // La recherche de ville part du navigateur, pas de la carte : elle remplit latitude et longitude
    "<script>q.onkeydown=e=>{if(e.key=='Enter'){e.preventDefault();s()}};async function s(){r.textContent='...';"
    "try{let j=await(await fetch('https://geocoding-api.open-meteo.com/v1/search?count=5&language=fr&name='+"
    "encodeURIComponent(q.value))).json();r.textContent=j.results?'':'Aucune ville trouvée';"
    "(j.results||[]).forEach(c=>{let b=document.createElement('button');b.type='button';"
    "b.textContent=[c.name,c.admin1,c.country_code].filter(x=>x).join(', ');"
    "b.onclick=()=>{lat.value=c.latitude;lon.value=c.longitude};r.append(b)})}"
    "catch(e){r.textContent='Recherche impossible'}}"
    // Le firmware part tel quel dans le corps de la requête ; la page se recharge après le redémarrage
    "function u(){if(!f.files[0])return;let x=new XMLHttpRequest();x.open('POST','/update');"
    "x.upload.onprogress=e=>m.textContent=Math.round(100*e.loaded/e.total)+' %';"
    "x.onload=()=>{m.textContent=x.responseText;setTimeout(()=>location.reload(),9000)};"
    "x.onerror=()=>m.textContent='Envoi interrompu';x.send(f.files[0])}</script>";

// Ajoute du texte à la page en cours de construction
static void add(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void add(const char *format, ...) {
  va_list args;
  va_start(args, format);
  page_len += vsnprintf(page + page_len, BUF_SIZE - page_len, format, args);
  va_end(args);
  page_len = min<size_t>(page_len, BUF_SIZE - 1);
}

static void reply(WiFiClient &c, const char *status, const char *text) {
  c.printf("HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
           status, (unsigned)strlen(text), text);
}

// Valeur du paramètre `name` de la requête, ou nul s'il est absent
static const char *param(const char *query, const char *name) {
  size_t len = strlen(name);
  for (const char *p = query; (p = strstr(p + 1, name)); p += len) {
    if ((p[-1] == '?' || p[-1] == '&') && p[len] == '=') return p + len + 1;
  }
  return nullptr;
}

// Une case décochée n'est pas envoyée par le navigateur : paramètre absent = désactivé
static void apply(const char *query) {
  for (uint8_t i = 0; i + 1 < pluginCount(); i++) {
    char name[8];
    snprintf(name, sizeof(name), "p%u", i);
    pluginSetEnabled(i, param(query, name) != nullptr);
  }

  WeatherSettings s;
  weatherSettings(s);
  s.enabled = param(query, "meteo") != nullptr;
  const char *lat = param(query, "lat"), *lon = param(query, "lon");
  if (lat && lon) {
    char *lat_end, *lon_end;
    float la = strtof(lat, &lat_end), lo = strtof(lon, &lon_end);
    if (lat_end != lat && lon_end != lon && fabsf(la) <= 90 && fabsf(lo) <= 180) s.latitude = la, s.longitude = lo;
  }
  weatherConfigure(s);

  BrightnessSettings b;
  brightnessSettings(b);
  const char *day = param(query, "jour"), *night = param(query, "nuit");
  if (day) b.day = constrain(atoi(day), 1, 100);
  if (night) b.night = constrain(atoi(night), 0, 100);
  b.automatic = param(query, "auto") != nullptr;
  brightnessConfigure(b);
}

// Octets de pile qu'une tâche n'a jamais utilisés, 0 si elle n'existe pas
static unsigned stackMargin(const char *task) {
  TaskHandle_t handle = xTaskGetHandle(task);
  return handle ? uxTaskGetStackHighWaterMark(handle) : 0;
}

static void sendPage(WiFiClient &c) {
  page_len = 0;
  add("%s", HEAD);
  // La dernière page est celle par défaut : elle ne se désactive pas
  for (uint8_t i = 0; i + 1 < pluginCount(); i++) {
    const char *name = pluginName(i);
    add("<label><input type=checkbox name=p%u%s> %c%s</label>", i, pluginEnabled(i) ? " checked" : "",
        toupper(name[0]), name + 1);
  }
  WeatherSettings s;
  weatherSettings(s);
  BrightnessSettings b;
  brightnessSettings(b);
  add(SETTINGS, s.enabled ? " checked" : "", b.day, b.day, b.automatic ? " checked" : "", b.night, b.night, s.latitude,
      s.longitude);

  char weather[40];
  int32_t age = weatherAge();
  if (!s.enabled) strlcpy(weather, "désactivée", sizeof(weather));
  else if (age < 0) strlcpy(weather, "pas encore lue", sizeof(weather));
  else snprintf(weather, sizeof(weather), "lue il y a %ld min", (long)(age / 60));
  unsigned long minutes = esp_timer_get_time() / 60000000;
  add(STATE, minutes / 60, minutes % 60, (int)WiFi.RSSI(), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
      (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned long)lcdBadFrames(),
      brightnessCurrent(), weather, esp_ota_get_running_partition()->label, stackMargin("web"), stackMargin("meteo"),
      stackMargin("sonos"));
  add("%s", SCRIPT);

  c.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
           (unsigned)page_len);
  c.write((const uint8_t *)page, page_len);
}

// Reçoit un firmware, envoyé tel quel dans le corps de la requête, et l'écrit dans la partition
// d'application qui ne sert pas. `data` : les `got` premiers octets, arrivés avec les en-têtes.
static void receiveFirmware(WiFiClient &c, uint8_t *data, size_t got, size_t total) {
  if (!total || !Update.begin(total)) {
    reply(c, "400 Bad Request", total ? Update.errorString() : "Fichier vide");
    return;
  }
  Serial.printf("Mise à jour : %u octets à recevoir\n", (unsigned)total);
  updating = true;
  delay(300);  // le temps que la boucle d'affichage passe l'écran au noir

  size_t done = 0;
  uint32_t last = millis();
  while (done < total && millis() - last < UPDATE_TIMEOUT_MS) {
    if (got == 0) {
      int n = c.read((uint8_t *)req, BUF_SIZE);
      if (n <= 0) {
        if (!c.connected() && !c.available()) break;
        delay(2);
        continue;
      }
      data = (uint8_t *)req;
      got = n;
    }
    got = min(got, total - done);
    if (Update.write(data, got) != got) break;
    done += got;
    got = 0;
    last = millis();
  }

  // Update.end() vérifie l'image avant de la désigner pour le prochain démarrage
  const char *error = done < total ? (Update.hasError() ? Update.errorString() : "Envoi incomplet") : nullptr;
  if (!error && !Update.end()) error = Update.errorString();
  Serial.printf("Mise à jour : %s\n", error ? error : "reçue, redémarrage");
  if (error) reply(c, "500 Internal Server Error", error);
  else reply(c, "200 OK", "Mise à jour reçue, redémarrage...");
  c.stop();
  // Echec compris : l'écran a été effacé, repartir de zéro est le plus simple
  delay(500);
  ESP.restart();
}

static void handle(WiFiClient &c) {
  // La requête jusqu'à la fin de ses en-têtes : sa première ligne, « GET /chemin?paramètres
  // HTTP/1.1 », dit quoi faire
  size_t len = 0;
  uint32_t t0 = millis();
  req[0] = 0;
  while (len < BUF_SIZE - 1 && millis() - t0 < HTTP_TIMEOUT_MS && c.connected() && !strstr(req, "\r\n\r\n")) {
    int got = c.read((uint8_t *)req + len, BUF_SIZE - 1 - len);
    if (got > 0) len += got;
    else delay(2);
    req[len] = 0;
  }
  char *end = strstr(req, " HTTP/"), *body = strstr(req, "\r\n\r\n");
  if (end) *end = 0;

  if (end && strncmp(req, "GET /set", 8) == 0) {
    apply(req + 7);
    // Retour à la page : la recharger ne renvoie pas les réglages une seconde fois
    c.print("HTTP/1.1 303 See Other\r\nLocation: /\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (end && strcmp(req, "GET /") == 0) {
    sendPage(c);
  } else if (end && body && strcmp(req, "POST /update") == 0) {
    const char *length = strcasestr(end + 1, "content-length:");
    body += 4;
    receiveFirmware(c, (uint8_t *)body, req + len - body, length ? strtoul(length + 15, nullptr, 10) : 0);
  } else {
    c.print("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  }
  c.stop();
}

static void webTask(void *) {
  WiFiServer server(80);
  bool listening = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      delay(500);
      continue;
    }
    if (!listening) {
      server.begin();
      listening = true;
      Serial.printf("Réglages : http://%s/\n", WiFi.localIP().toString().c_str());
    }
    WiFiClient c = server.accept();
    if (c) handle(c);
    else delay(50);
  }
}

void webBegin() {
  req = (char *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
  page = (char *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (req && page) xTaskCreatePinnedToCore(webTask, "web", 4608, nullptr, 1, nullptr, 0);
}

bool webUpdating() {
  return updating;
}
