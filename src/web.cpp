#include "web.h"
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include "astro.h"
#include "brightness.h"
#include "gfx.h"
#include "lcd.h"
#include "plugin.h"
#include "weather.h"

#define BUF_SIZE           6144  // une requête de navigateur fait moins de 1 Ko, la page ~4,5 Ko
#define HTTP_TIMEOUT_MS    1500  // pour recevoir une requête entière
#define IDLE_TIMEOUT_MS    600   // pour en recevoir le début
#define UPDATE_TIMEOUT_MS  10000  // silence maximal pendant l'envoi d'un firmware

static char *req, *page;  // requête reçue et page envoyée, en PSRAM
static size_t page_len;
static volatile bool updating = false;

// La page ressemble à un écran de réglages : des sections arrondies, une ligne par réglage, le
// libellé à gauche et la commande à droite. Chaque case ou curseur est enregistré dès qu'il
// change (voir SCRIPT) ; seul le lieu attend son bouton.
static const char HEAD[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Écran</title><style>body{font:17px system-ui;max-width:26em;margin:0 auto;padding:1em;background:#000;"
    "color:#eee}h1{font-size:1.5em;margin:.4em .6em}h2{font:600 .8em system-ui;color:#999;text-transform:uppercase;"
    "margin:1.8em 1.2em .5em}section{background:#1c1c1e;border-radius:12px;padding:0 1em}"
    "section>*{display:flex;align-items:center;gap:.7em;min-height:2.9em;border-top:1px solid #333}"
    "section>:first-child{border:0}input[type=checkbox]{width:1.3em;height:1.3em;margin-left:auto;accent-color:#30d158}"
    "input[type=range]{flex:1;min-width:0;accent-color:#0a84ff}input:not([type]),button{font:inherit;color:#fff;"
    "background:#2c2c2e;border:0;border-radius:8px;padding:.45em .7em}input:not([type]){flex:1;min-width:0}input[type=time]{font:inherit;color:#fff;background:#2c2c2e;border:0;border-radius:8px;padding:.3em .5em;margin-left:auto;color-scheme:dark}"
    "button{background:#0a84ff}#r{flex-wrap:wrap;padding:.5em 0}#r:empty{display:none}#r button{background:#2c2c2e}"
    "input[type=file]{font:inherit;color:#999;min-width:0}b{margin-left:auto;font-weight:400;color:#999;text-align:right}"
    "small{font-size:.6em;font-weight:400;color:#999}p{font-size:.8em;color:#999;margin:.5em 1.2em}</style>"
    "<h1>Écran <small id=ok></small></h1><form><h2>Affichage</h2><section>";

// Une page activable. Arguments : nom (initiale, suite), rang, cochée ou non
static const char PAGE_ROW[] = "<label>%c%s<input type=checkbox name=p%u%s></label>";

// Arguments : météo, soleil et lune cochés ou non, luminosité de jour (deux fois), cycle coché
// ou non, luminosité de nuit (deux fois), rouge coché ou non, extinction cochée ou non, heures
// et minutes d'extinction puis de rallumage, veille profonde cochée ou non, latitude, longitude
static const char SETTINGS[] =
    "<label>Météo<input type=checkbox name=meteo%s></label>"
    "<label>Lever et coucher du soleil<input type=checkbox name=soleil%s></label>"
    "<label>Phase de la lune<input type=checkbox name=lune%s></label></section>"
    "<p>L'horloge reste toujours affichée."
    "<h2>Luminosité</h2><section>"
    "<label>Jour<input type=range name=jour min=1 max=100 value=%u><b>%u %%</b></label>"
    "<label>Cycle automatique<input type=checkbox name=auto%s></label>"
    "<label>Nuit<input type=range name=nuit min=0 max=100 value=%u><b>%u %%</b></label>"
    "<label>Rouge sombre la nuit<input type=checkbox name=rouge%s></label>"
    "<label>Extinction programmée<input type=checkbox name=veille%s></label>"
    "<label>Éteindre à<input type=time name=debut value=%02u:%02u></label>"
    "<label>Rallumer à<input type=time name=fin value=%02u:%02u></label>"
    "<label>Veille profonde pendant l'extinction<input type=checkbox name=profonde%s></label></section>"
    "<p>Le cycle passe du niveau de jour au niveau de nuit quand le soleil se couche. Sans lui, seul le niveau de "
    "jour sert. Le rouge suit le soleil de la même façon, avec ou sans le cycle. L'extinction programmée éteint "
    "tout à fait l'écran entre les deux heures, chaque jour."
    "<p>En veille profonde, la carte s'arrête aussi pour consommer moins : cette page ne répond plus jusqu'à l'heure "
    "du rallumage. Pour la joindre avant, appuyer sur son bouton reset : elle reste éveillée 5 minutes."
    "<h2>Lieu de la météo et de l'heure</h2><section>"
    "<div><input id=q placeholder='Chercher une ville'><button type=button onclick=s()>Chercher</button></div>"
    "<div id=r></div><label>Latitude<input id=lat name=lat value=%.4f></label>"
    "<label>Longitude<input id=lon name=lon value=%.4f></label>"
    "<div><button>Enregistrer le lieu</button></div></section></form>"
    "<h2>Mise à jour du firmware</h2><section><div><input type=file id=f accept=.bin></div>"
    "<div><button type=button onclick=u()>Envoyer</button><span id=m></span></div></section>";

// Arguments : heure locale, soleil et lune, heures et minutes de fonctionnement, cause du démarrage, signal Wi-Fi,
// RAM interne libre et son minimum, images ratées, luminosité, teinte de nuit, météo, partition,
// marge des piles web, météo et Sonos
static const char STATE[] =
    "<h2>État</h2><section><div>Heure locale<b>%02d:%02d</b></div><div>Soleil et lune<b>%s</b></div>"
    "<div>Allumé depuis<b>%lu h %02lu min</b></div><div>Dernier démarrage<b>%s</b></div><div>Wi-Fi<b>%d dBm</b></div>"
    "<div>RAM interne libre<b>%u Ko, au plus bas %u Ko</b></div><div>Images ratées<b>%lu</b></div>"
    "<div>Luminosité<b>%u %%, rouge %u/%u</b></div><div>Météo<b>%s</b></div><div>Firmware<b>partition %s</b></div>"
    "<div>Marge des piles<b>web %u, météo %u, Sonos %u</b></div></section>";

static const char SCRIPT[] =
    "<script>let F=document.forms[0];"
    // Tout réglage part dès qu'il change, sans recharger la page. Le lieu attend son bouton : ses
    // deux champs n'ont de sens qu'ensemble.
    "async function save(){ok.textContent='...';try{let a=await fetch('/set?'+new URLSearchParams(new FormData(F)));"
    "ok.textContent=a.ok?'enregistré':'refusé'}catch(e){ok.textContent='carte injoignable'}}"
    "F.onchange=e=>{if(e.target.type!='text')save()};F.onsubmit=e=>{e.preventDefault();save()};"
    "F.oninput=e=>{if(e.target.type=='range')e.target.nextElementSibling.textContent=e.target.value+' %'};"
    // La recherche de ville part du navigateur, pas de la carte ; choisir une ville l'enregistre
    "q.onkeydown=e=>{if(e.key=='Enter'){e.preventDefault();s()}};async function s(){r.textContent='...';"
    "try{let j=await(await fetch('https://geocoding-api.open-meteo.com/v1/search?count=5&language=fr&name='+"
    "encodeURIComponent(q.value))).json();r.textContent=j.results?'':'Aucune ville trouvée';"
    "(j.results||[]).forEach(c=>{let b=document.createElement('button');b.type='button';"
    "b.textContent=[c.name,c.admin1,c.country_code].filter(x=>x).join(', ');"
    "b.onclick=()=>{lat.value=c.latitude;lon.value=c.longitude;r.textContent='';save()};r.append(b)})}"
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

// Minutes depuis minuit d'une heure "HH:MM", dont le deux-points arrive codé en %3A. -1 si elle
// est mal formée.
static int minuteOfDay(const char *value) {
  if (!value) return -1;
  char *end;
  long hours = strtol(value, &end, 10);
  if (end == value) return -1;
  if (*end == ':') end++;
  else if (strncasecmp(end, "%3A", 3) == 0) end += 3;
  else return -1;
  long minutes = strtol(end, nullptr, 10);
  return hours >= 0 && hours < 24 && minutes >= 0 && minutes < 60 ? hours * 60 + minutes : -1;
}

// La page envoie tous ses réglages à chaque fois. Une case décochée n'est pas envoyée : paramètre
// absent = désactivé.
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
  b.red = param(query, "rouge") != nullptr;
  b.sleep = param(query, "veille") != nullptr;
  int from = minuteOfDay(param(query, "debut")), to = minuteOfDay(param(query, "fin"));
  if (from >= 0) b.sleep_from = from;
  if (to >= 0) b.sleep_to = to;
  b.deep = param(query, "profonde") != nullptr;
  brightnessConfigure(b);

  astroConfigure({param(query, "soleil") != nullptr, param(query, "lune") != nullptr});
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
    add(PAGE_ROW, toupper(name[0]), name + 1, i, pluginEnabled(i) ? " checked" : "");
  }
  WeatherSettings s;
  weatherSettings(s);
  BrightnessSettings b;
  brightnessSettings(b);
  AstroSettings sky;
  astroSettings(sky);
  add(SETTINGS, s.enabled ? " checked" : "", sky.sun ? " checked" : "", sky.moon ? " checked" : "", b.day, b.day,
      b.automatic ? " checked" : "", b.night, b.night, b.red ? " checked" : "", b.sleep ? " checked" : "",
      b.sleep_from / 60, b.sleep_from % 60, b.sleep_to / 60, b.sleep_to % 60, b.deep ? " checked" : "", s.latitude,
      s.longitude);

  char weather[40];
  int32_t age = weatherAge();
  if (!s.enabled) strlcpy(weather, "désactivée", sizeof(weather));
  else if (age < 0) strlcpy(weather, "pas encore lue", sizeof(weather));
  else snprintf(weather, sizeof(weather), "lue il y a %ld min", (long)(age / 60));
  unsigned long minutes = esp_timer_get_time() / 60000000;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  // Soleil du jour et lune, tels que la carte les calcule
  char sun[80] = "ni lever ni coucher";
  struct tm noon = t;
  noon.tm_hour = 12;
  noon.tm_min = noon.tm_sec = 0;
  time_t rise, set;
  if (sunTimes(mktime(&noon), s.latitude, s.longitude, rise, set) == SUN_RISES) {
    struct tm r, e;
    rise += 30, set += 30;
    localtime_r(&rise, &r);
    localtime_r(&set, &e);
    snprintf(sun, sizeof(sun), "%02d:%02d à %02d:%02d, phase %u/8", r.tm_hour, r.tm_min, e.tm_hour, e.tm_min,
             moonPhase(now));
  }
  const char *boot;
  switch (esp_reset_reason()) {
    case ESP_RST_DEEPSLEEP: boot = "réveil programmé"; break;
    case ESP_RST_POWERON: boot = "mise sous tension ou reset"; break;
    case ESP_RST_SW: boot = "redémarrage demandé"; break;
    case ESP_RST_PANIC: boot = "plantage"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: boot = "chien de garde"; break;
    case ESP_RST_BROWNOUT: boot = "baisse de tension"; break;
    default: boot = "autre"; break;
  }
  add(STATE, t.tm_hour, t.tm_min, sun, minutes / 60, minutes % 60, boot, (int)WiFi.RSSI(),
      (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
      (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned long)lcdBadFrames(),
      brightnessCurrent(), gfxNight(), GFX_NIGHT_MAX, weather, esp_ota_get_running_partition()->label, stackMargin("web"),
      stackMargin("meteo"), stackMargin("sonos"));
  add("%s", SCRIPT);

  // Jamais gardée en cache par le navigateur : elle doit montrer les réglages du moment
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n"
           "Content-Length: %u\r\nConnection: close\r\n\r\n",
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
  while (len < BUF_SIZE - 1 && millis() - t0 < (len ? HTTP_TIMEOUT_MS : IDLE_TIMEOUT_MS) && c.connected() &&
         !strstr(req, "\r\n\r\n")) {
    int got = c.read((uint8_t *)req + len, BUF_SIZE - 1 - len);
    if (got > 0) len += got;
    else delay(2);
    req[len] = 0;
  }
  char *end = strstr(req, " HTTP/"), *body = strstr(req, "\r\n\r\n");
  // Les navigateurs ouvrent des connexions d'avance, sans rien y envoyer. On les referme sans
  // répondre : une réponse y resterait en attente, et le navigateur la prendrait pour celle de la
  // requête qu'il finit par y envoyer. Fermée, il en ouvre simplement une autre.
  if (!end || !body) {
    c.stop();
    return;
  }
  *end = 0;

  if (end && strncmp(req, "GET /set", 8) == 0) {
    apply(req + 7);
    // La page envoie les réglages sans se recharger : rien à lui répondre
    c.print("HTTP/1.1 204 No Content\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");
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
