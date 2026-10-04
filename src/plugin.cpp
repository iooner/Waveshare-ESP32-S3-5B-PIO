#include "plugin.h"

static const Plugin *bar;
static const Plugin *const *pages;
static uint8_t count;
static int8_t current = -1;

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
  count = n;
  if (bar->begin) bar->begin();
  for (uint8_t i = 0; i < count; i++) {
    if (pages[i]->begin) pages[i]->begin();
  }
  pluginsLoop();
}

void pluginsLoop() {
  uint8_t wanted = 0;
  while (wanted < count - 1 && pages[wanted]->active && !pages[wanted]->active()) wanted++;
  if (wanted != current) {
    showPage(wanted);
    return;
  }
  const Plugin *p = pages[current];
  if (!p->fullscreen && bar->update) bar->update();
  if (p->update) p->update();
}
