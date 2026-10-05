#include "plugin.h"
#include <Preferences.h>

#define MAX_PAGES  8

static const Plugin *bar;
static const Plugin *const *pages;
static uint8_t count;
static int8_t current = -1;
static uint8_t drawn_night;      // teinte de nuit de ce qui est à l'écran
static bool enabled[MAX_PAGES];  // modifié par le back office, depuis une autre tâche
static PluginRotation rotation = {false, 30, 10};  // idem
static bool home_turn;           // alternance : c'est le tour de la page par défaut
static uint32_t turn_at;         // début du tour en cours

static void showPage(uint8_t index) {
  if (current >= 0 && pages[current]->hide) pages[current]->hide();
  current = index;
  Serial.printf("Page : %s\n", pages[index]->name);
  // L'écran est effacé et présenté avant que la page ne dessine : ce gros transfert en PSRAM se
  // fait ainsi pendant que rien à l'écran n'en dépend (une page peut afficher une image)
  gfxClear(COLOR_BG);
  lcdPresent();
  if (!pages[index]->fullscreen) bar->show();
  pages[index]->show();
}

void pluginsBegin(const Plugin *b, const Plugin *const *p, uint8_t n) {
  bar = b;
  pages = p;
  count = min<uint8_t>(n, MAX_PAGES);
  Preferences prefs;
  prefs.begin("pages");
  for (uint8_t i = 0; i < count; i++) enabled[i] = prefs.getBool(pages[i]->name, !pages[i]->optional);
  rotation.enabled = prefs.getBool("alterne", rotation.enabled);
  rotation.page_seconds = prefs.getUShort("t_page", rotation.page_seconds);
  rotation.home_seconds = prefs.getUShort("t_accueil", rotation.home_seconds);
  prefs.end();

  if (bar->begin) bar->begin();
  for (uint8_t i = 0; i < count; i++) {
    if (pages[i]->begin) pages[i]->begin();
  }
  pluginsLoop();
}

void pluginsLoop() {
  uint8_t wanted = 0;
  while (wanted < count - 1 && (!enabled[wanted] || (pages[wanted]->active && !pages[wanted]->active()))) wanted++;

  // Alternance, pour une page qui n'a pas toujours quelque chose à montrer : elle commence par
  // son propre tour, puis cède l'écran à la page par défaut, et ainsi de suite
  PluginRotation r = rotation;
  if (r.enabled && wanted < count - 1 && pages[wanted]->active) {
    if (millis() - turn_at >= (home_turn ? r.home_seconds : r.page_seconds) * 1000UL) {
      home_turn = !home_turn;
      turn_at = millis();
    }
    if (home_turn) wanted = count - 1;
  } else {
    home_turn = false;
    turn_at = millis();
  }

  if (wanted != current) {
    drawn_night = gfxNight();
    showPage(wanted);
    return;
  }
  const Plugin *p = pages[current];
  // Teinte de nuit changée : la page se redessine par-dessus elle-même. Sans effacer, pour que
  // l'écran ne passe pas par le noir à chaque pas du fondu.
  if (gfxNight() != drawn_night) {
    drawn_night = gfxNight();
    if (!p->fullscreen) bar->show();
    p->show();
    return;
  }
  if (!p->fullscreen && bar->update) bar->update();
  if (p->update) p->update();
}

uint8_t pluginCount() {
  return count;
}

const char *pluginName(uint8_t index) {
  return pages[index]->name;
}

const char *pluginCurrentName() {
  return current >= 0 ? pages[current]->name : "";
}

bool pluginEnabled(uint8_t index) {
  return enabled[index];
}

void pluginRotation(PluginRotation &out) {
  out = rotation;
}

void pluginSetRotation(const PluginRotation &r) {
  PluginRotation n = {r.enabled, constrain(r.page_seconds, (uint16_t)5, (uint16_t)3600),
                      constrain(r.home_seconds, (uint16_t)5, (uint16_t)3600)};
  if (n.enabled == rotation.enabled && n.page_seconds == rotation.page_seconds && n.home_seconds == rotation.home_seconds) return;
  rotation = n;
  Preferences prefs;
  prefs.begin("pages");
  prefs.putBool("alterne", n.enabled);
  prefs.putUShort("t_page", n.page_seconds);
  prefs.putUShort("t_accueil", n.home_seconds);
  prefs.end();
}

void pluginSetEnabled(uint8_t index, bool on) {
  if (index >= count || enabled[index] == on) return;
  enabled[index] = on;
  Preferences prefs;
  prefs.begin("pages");
  prefs.putBool(pages[index]->name, on);
  prefs.end();
}
