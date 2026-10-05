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
  void (*show)();    // dessine toute sa zone. Elle est effacée quand la page arrive, mais pas quand la teinte de
                     // nuit change : tout doit être repeint, fond compris (gfxTextBox le fait).
  void (*update)();  // à chaque image tant qu'il est affiché : ne redessiner que ce qui change. Peut être nul.
  void (*hide)();    // avant de céder la place. Peut être nul.
  bool optional;     // désactivée tant qu'on ne l'active pas dans le back office (web.h)
  bool exclusive;    // garde l'écran quand elle a quelque chose à montrer, au lieu de prendre son tour dans le
                     // diaporama. Pour une page non optionnelle, ce n'est que le réglage de départ.
  const char *label; // nom affiché dans le back office. Nul : `name`, avec une majuscule.
};

// Initialise tous les plugins. La dernière page de la liste, l'accueil, doit être toujours active.
//
// Quelle page est à l'écran : la première de la liste qui est exclusive et a quelque chose à
// montrer ; à défaut, les pages qui ont quelque chose à montrer se succèdent en diaporama,
// chacune pendant sa durée. Une page qui vient d'avoir quelque chose à montrer passe tout de suite.
void pluginsBegin(const Plugin *bar, const Plugin *const *pages, uint8_t count);

// A appeler à chaque image, avant lcdPresent()
void pluginsLoop();

// Pages, dans l'ordre de la liste, et leurs réglages, gardés en flash
struct PluginSlide {
  bool enabled;      // une page désactivée n'est jamais affichée ; l'accueil ne se désactive pas
  bool exclusive;    // voir Plugin::exclusive
  uint16_t seconds;  // durée de son tour dans le diaporama
};
uint8_t pluginCount();
const Plugin *pluginAt(uint8_t index);

// Fondu au noir entre deux pages, au lieu d'un changement sec. Gardé en flash.
bool pluginFade();
void pluginSetFade(bool on);
const char *pluginCurrentName();  // la page à l'écran
void pluginSlide(uint8_t index, PluginSlide &out);
void pluginSetSlide(uint8_t index, const PluginSlide &s);

// Plugins disponibles
extern const Plugin clock_bar_plugin;  // barre : date à gauche, heure à droite
extern const Plugin sonos_plugin;      // page : morceau en cours sur les enceintes Sonos, quand elles jouent
extern const Plugin crypto_plugin;     // page : cours des cryptomonnaies choisies et valeur du portefeuille
extern const Plugin agenda_plugin;     // page : prochains événements du Liège Hackerspace
extern const Plugin my_agenda_plugin;  // page : prochains événements des agendas personnels
extern const Plugin clock_plugin;      // page plein écran : heure et date en grand, météo des heures à venir
extern const Plugin demo_plugin;       // page : mire de test (couleurs, texte, carré animé, FPS)
