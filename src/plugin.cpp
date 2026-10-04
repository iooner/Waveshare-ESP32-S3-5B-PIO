#include "plugin.h"

static const Plugin *const *list;
static uint8_t count, current;
static uint32_t shown_at;

static void showPlugin(uint8_t index) {
  current = index;
  shown_at = millis();
  Serial.printf("Plugin : %s\n", list[index]->name);
  list[index]->show();
}

void pluginsBegin(const Plugin *const *plugins, uint8_t n) {
  list = plugins;
  count = n;
  for (uint8_t i = 0; i < count; i++) {
    if (list[i]->begin) list[i]->begin();
  }
  showPlugin(0);
}

void pluginsLoop() {
  const Plugin *p = list[current];
  if (count > 1 && p->seconds && millis() - shown_at >= p->seconds * 1000UL) {
    showPlugin((current + 1) % count);
  } else if (p->update) {
    p->update();
  }
}
