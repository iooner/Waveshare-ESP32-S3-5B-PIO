#include "plugin.h"

static const Plugin *bar;
static const Plugin *const *pages;
static uint8_t count, current;
static uint32_t shown_at;

static void showPage(uint8_t index) {
  current = index;
  shown_at = millis();
  Serial.printf("Page : %s\n", pages[index]->name);
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
  bar->show();
  showPage(0);
}

void pluginsLoop() {
  if (bar->update) bar->update();
  const Plugin *p = pages[current];
  if (count > 1 && p->seconds && millis() - shown_at >= p->seconds * 1000UL) {
    showPage((current + 1) % count);
  } else if (p->update) {
    p->update();
  }
}
