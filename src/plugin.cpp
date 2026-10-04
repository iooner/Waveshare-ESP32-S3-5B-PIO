#include "plugin.h"
#include <Preferences.h>

#define MAX_PAGES  8

static const Plugin *bar;
static const Plugin *const *pages;
static uint8_t count;
static int8_t current = -1;
static bool enabled[MAX_PAGES];  // modifié par le back office, depuis une autre tâche

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
  if (wanted != current) {
    showPage(wanted);
    return;
  }
  const Plugin *p = pages[current];
  if (!p->fullscreen && bar->update) bar->update();
  if (p->update) p->update();
}

uint8_t pluginCount() {
  return count;
}

const char *pluginName(uint8_t index) {
  return pages[index]->name;
}

bool pluginEnabled(uint8_t index) {
  return enabled[index];
}

void pluginSetEnabled(uint8_t index, bool on) {
  if (index >= count || enabled[index] == on) return;
  enabled[index] = on;
  Preferences prefs;
  prefs.begin("pages");
  prefs.putBool(pages[index]->name, on);
  prefs.end();
}
