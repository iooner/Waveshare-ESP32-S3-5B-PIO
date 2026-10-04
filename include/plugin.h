// Système de plugins : un plugin est une page plein écran, décrite par une struct Plugin.
// Un seul est affiché à la fois ; s'il y en a plusieurs, ils défilent.
//
// Ajouter un plugin :
//   1. src/plugins/monplugin.cpp : définir `const Plugin mon_plugin = {...};`
//   2. le déclarer en bas de ce fichier
//   3. l'ajouter à la liste `plugins` de src/main.cpp
#pragma once
#include <Arduino.h>
#include "gfx.h"

// Couleurs communes à tous les plugins
#define COLOR_BG    COLOR_BLACK
#define COLOR_TEXT  COLOR_WHITE

struct Plugin {
  const char *name;
  uint16_t seconds;  // durée à l'écran quand plusieurs plugins défilent. 0 = reste affiché.
  void (*begin)();   // une fois au démarrage, avant tout affichage. Peut être nul.
  void (*show)();    // le plugin prend l'écran : il doit le redessiner en entier
  void (*update)();  // à chaque image tant qu'il est affiché : ne redessiner que ce qui change. Peut être nul.
};

// Initialise tous les plugins et affiche le premier
void pluginsBegin(const Plugin *const *plugins, uint8_t count);

// A appeler à chaque image, avant lcdPresent()
void pluginsLoop();

// Plugins disponibles
extern const Plugin clock_plugin;  // date et heure
extern const Plugin demo_plugin;   // mire de test : couleurs, texte, carré animé, FPS
