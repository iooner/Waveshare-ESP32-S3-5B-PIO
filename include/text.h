// Texte à afficher : mise au format des polices, noms de jours et de mois
#pragma once
#include <Arduino.h>

// Les polices couvrent le Latin-1 : remplace sur place les caractères typographiques courants qui
// n'y sont pas (apostrophe courbe, guillemets anglais, tirets longs, oe lié...)
void toLatin1(char *s);

// Retire sur place ce que les polices ne savent pas dessiner (emoji, autres écritures), puis les
// espaces en trop que cela laisse. A faire après toLatin1().
void dropUnsupported(char *s);

extern const char *const DAY_NAMES[7];     // "dimanche"...
extern const char *const MONTH_NAMES[12];  // "janvier"...
extern const char *const MONTH_SHORT[12];  // "janv."...
