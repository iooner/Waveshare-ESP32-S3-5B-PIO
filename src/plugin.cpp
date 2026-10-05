#include "plugin.h"
#include <Preferences.h>

#define MAX_PAGES  8
#define FADE_STEP  1  // niveaux de clarté gagnés ou perdus par image : un fondu dure 9 images, soit un quart de seconde

static const Plugin *bar;
static const Plugin *const *pages;
static uint8_t count;
static int8_t current = -1;
static uint8_t drawn_night;      // teinte de nuit de ce qui est à l'écran
static PluginSlide slides[MAX_PAGES];  // modifiés par le back office, depuis une autre tâche
static uint8_t slide;         // diaporama : la page dont c'est le tour
static uint32_t turn_at;      // début de ce tour
static uint8_t was_showable;  // pages qui avaient quelque chose à montrer à l'image précédente, un bit chacune
static bool fade_on = true;   // fondu entre les pages, réglé dans le back office
static uint8_t fade_level = LCD_FADE_MAX;  // clarté demandée à l'écran ; remonte d'elle-même après un changement de page

// Clé de flash d'un réglage de page : son nom, précédé d'une lettre
static const char *key(char prefix, uint8_t index) {
  static char text[16];
  snprintf(text, sizeof(text), "%c_%s", prefix, pages[index]->name);
  return text;
}

static void showPage(uint8_t index) {
  // Fondu : la page en place s'éteint, puis tout le changement se fait dans le noir, où le
  // balayage ne lit plus la PSRAM. La nouvelle page s'éclaire ensuite image après image, depuis
  // pluginsLoop(). Pas de fondu pour la toute première page.
  if (fade_on && current >= 0) {
    while (fade_level > 0) {
      fade_level = fade_level > FADE_STEP ? fade_level - FADE_STEP : 0;
      lcdSetFade(fade_level);
      lcdPresent();  // rien à afficher de neuf : attend l'image suivante
    }
    lcdPresent();  // le noir est maintenant en cours de balayage
  }
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
  // Réglages de la version précédente, où seule une page exclusive pouvait alterner avec l'accueil
  bool alternated = prefs.getBool("alterne", false);
  for (uint8_t i = 0; i < count; i++) {
    const Plugin *page = pages[i];
    bool home = i == count - 1, was_exclusive = page->exclusive && !page->optional;
    uint16_t seconds = home ? prefs.getUShort("t_accueil", 15) : was_exclusive ? prefs.getUShort("t_page", 15) : 15;
    slides[i].enabled = home || prefs.getBool(page->name, !page->optional);
    slides[i].exclusive = !home && prefs.getBool(key('x', i), page->exclusive && !(was_exclusive && alternated));
    slides[i].seconds = prefs.getUShort(key('t', i), seconds);
  }
  slide = count - 1;
  fade_on = prefs.getBool("fondu", fade_on);
  prefs.end();

  if (bar->begin) bar->begin();
  for (uint8_t i = 0; i < count; i++) {
    if (pages[i]->begin) pages[i]->begin();
  }
  pluginsLoop();
}

// La page a-t-elle sa place à l'écran en ce moment ? L'accueil, toujours.
static bool showable(uint8_t i) {
  return i == count - 1 || (slides[i].enabled && (!pages[i]->active || pages[i]->active()));
}

void pluginsLoop() {
  if (fade_level < LCD_FADE_MAX) {
    fade_level = min<uint8_t>(fade_level + FADE_STEP, LCD_FADE_MAX);
    lcdSetFade(fade_level);
  }

  uint8_t now_showable = 0, wanted = count;
  for (uint8_t i = 0; i < count; i++) {
    if (!showable(i)) continue;
    now_showable |= 1 << i;
    if (wanted == count && slides[i].exclusive) wanted = i;
  }
  if (wanted < count) {
    turn_at = millis();  // après une page exclusive, le diaporama reprend par un tour complet
  } else {
    uint8_t fresh = now_showable & ~was_showable;
    if (fresh) {
      slide = __builtin_ctz(fresh);
      turn_at = millis();
    } else if (!(now_showable >> slide & 1) ||
               (now_showable != 1 << slide && millis() - turn_at >= slides[slide].seconds * 1000UL)) {
      do slide = (slide + 1) % count;
      while (!(now_showable >> slide & 1));
      turn_at = millis();
    }
    wanted = slide;
  }
  was_showable = now_showable;

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

bool pluginFade() {
  return fade_on;
}

void pluginSetFade(bool on) {
  if (on == fade_on) return;
  fade_on = on;
  Preferences prefs;
  prefs.begin("pages");
  prefs.putBool("fondu", on);
  prefs.end();
}

const Plugin *pluginAt(uint8_t index) {
  return pages[index];
}

const char *pluginCurrentName() {
  return current >= 0 ? pages[current]->name : "";
}

void pluginSlide(uint8_t index, PluginSlide &out) {
  out = slides[index];
}

void pluginSetSlide(uint8_t index, const PluginSlide &s) {
  if (index >= count) return;
  bool home = index == count - 1;
  PluginSlide n = {home || s.enabled, !home && s.exclusive, constrain(s.seconds, (uint16_t)5, (uint16_t)3600)};
  PluginSlide &o = slides[index];
  if (n.enabled == o.enabled && n.exclusive == o.exclusive && n.seconds == o.seconds) return;
  o = n;
  Preferences prefs;
  prefs.begin("pages");
  prefs.putBool(pages[index]->name, n.enabled);
  prefs.putBool(key('x', index), n.exclusive);
  prefs.putUShort(key('t', index), n.seconds);
  prefs.end();
}
