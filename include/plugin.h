// Système de plugins. L'écran a deux zones :
//   - la barre du haut (BAR_HEIGHT pixels), tenue par un plugin ;
//   - la page, en dessous. La page affichée est la première de la liste qui a quelque chose à
//     montrer ; une page plein écran prend aussi la place de la barre.
//
// Ajouter une page :
//   1. src/plugins/monplugin.cpp : définir `const Plugin mon_plugin = {...};`
//   2. le déclarer en bas de ce fichier
//   3. l'ajouter à la liste `pages` de src/main.cpp, à son rang de priorité
#pragma once
#include <Arduino.h>
#include "gfx.h"
#include "lcd.h"

// Couleurs communes à tous les plugins
#define COLOR_BG    COLOR_BLACK
#define COLOR_TEXT  COLOR_WHITE
#define COLOR_DIM   RGB565(150, 150, 150)  // texte secondaire

// Géométrie commune
#define BAR_HEIGHT   80
#define PAGE_Y       BAR_HEIGHT
#define PAGE_HEIGHT  (LCD_HEIGHT - BAR_HEIGHT)
#define MARGIN_X     48
#define CONTENT_W    (LCD_WIDTH - 2 * MARGIN_X)

struct Plugin {
  const char *name;
  bool fullscreen;   // la page occupe aussi la barre
  void (*begin)();   // une fois au démarrage, avant tout affichage. Peut être nul.
  bool (*active)();  // la page a-t-elle quelque chose à montrer ? Nul = toujours.
  void (*show)();    // le plugin prend sa zone, déjà effacée : il la dessine en entier
  void (*update)();  // à chaque image tant qu'il est affiché : ne redessiner que ce qui change. Peut être nul.
  void (*hide)();    // avant de céder la place. Peut être nul.
  bool optional;     // désactivée tant qu'on ne l'active pas dans le back office (web.h)
};

// Initialise tous les plugins. La dernière page de la liste doit être toujours active.
void pluginsBegin(const Plugin *bar, const Plugin *const *pages, uint8_t count);

// A appeler à chaque image, avant lcdPresent()
void pluginsLoop();

// Pages, dans l'ordre de priorité. Une page désactivée n'est jamais affichée ; le choix est
// gardé en flash. La dernière page reste toujours active.
uint8_t pluginCount();
const char *pluginName(uint8_t index);
bool pluginEnabled(uint8_t index);
void pluginSetEnabled(uint8_t index, bool on);

// Plugins disponibles
extern const Plugin clock_bar_plugin;  // barre : date à gauche, heure à droite
extern const Plugin sonos_plugin;      // page : morceau en cours sur les enceintes Sonos, quand elles jouent
extern const Plugin clock_plugin;      // page plein écran : heure et date en grand, météo des heures à venir
extern const Plugin demo_plugin;       // page : mire de test (couleurs, texte, carré animé, FPS)
