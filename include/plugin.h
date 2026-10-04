// Système de plugins. L'écran a deux zones :
//   - la barre du haut (BAR_HEIGHT pixels), tenue par un plugin toujours affiché ;
//   - la page, en dessous, tenue par un plugin à la fois ; s'il y en a plusieurs, ils défilent.
//
// Ajouter une page :
//   1. src/plugins/monplugin.cpp : définir `const Plugin mon_plugin = {...};`
//   2. le déclarer en bas de ce fichier
//   3. l'ajouter à la liste `pages` de src/main.cpp
#pragma once
#include <Arduino.h>
#include "gfx.h"
#include "lcd.h"

// Couleurs communes à tous les plugins
#define COLOR_BG    COLOR_BLACK
#define COLOR_TEXT  COLOR_WHITE
#define COLOR_DIM   RGB565(150, 150, 150)  // texte secondaire

// Géométrie commune
#define BAR_HEIGHT   64
#define PAGE_Y       BAR_HEIGHT
#define PAGE_HEIGHT  (LCD_HEIGHT - BAR_HEIGHT)
#define MARGIN_X     48
#define CONTENT_W    (LCD_WIDTH - 2 * MARGIN_X)

struct Plugin {
  const char *name;
  uint16_t seconds;  // durée à l'écran quand plusieurs pages défilent. 0 = reste affichée.
  void (*begin)();   // une fois au démarrage, avant tout affichage. Peut être nul.
  void (*show)();    // le plugin prend sa zone : il doit la redessiner en entier
  void (*update)();  // à chaque image tant qu'il est affiché : ne redessiner que ce qui change. Peut être nul.
};

// Initialise tous les plugins, affiche la barre et la première page
void pluginsBegin(const Plugin *bar, const Plugin *const *pages, uint8_t count);

// A appeler à chaque image, avant lcdPresent()
void pluginsLoop();

// Plugins disponibles
extern const Plugin clock_plugin;  // barre : date à gauche, heure à droite
extern const Plugin sonos_plugin;  // page : morceau en cours sur les enceintes Sonos
extern const Plugin demo_plugin;   // page : mire de test (couleurs, texte, carré animé, FPS)
