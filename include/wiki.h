// L'article mis en avant chaque jour par Wikipédia en français, rubrique « Lumière sur » de sa page
// d'accueil : son titre, le début de son introduction et l'image choisie par la rubrique. Lu une
// fois par jour sur fr.wikipedia.org, sans compte ni clé. Nécessite netBegin().
#pragma once
#include <Arduino.h>

#define WIKI_IMAGE_MAX  300  // l'image tient dans un carré de ce côté

struct WikiArticle {
  char title[128];  // au format des polices (text.h)
  char text[800];   // le début de l'introduction, au format des polices
  const uint16_t *image;  // RGB565, image_w x image_h pixels ; nul s'il n'y en a pas
  uint16_t image_w, image_h;
};

// Lance la lecture en arrière-plan : au démarrage, puis à chaque changement de jour
void wikiBegin();

// Version de l'article, qui change quand il change. 0 s'il n'y a rien à montrer.
uint32_t wikiVersion();

// Article du jour. L'image reste valable jusqu'au changement de version suivant.
void wikiGet(WikiArticle &out);

// Secondes écoulées depuis la dernière lecture réussie, ou -1 s'il n'y en a pas eu
int32_t wikiAge();
