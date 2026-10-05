// Texte à afficher : mise au format des polices, noms de jours et de mois
#pragma once
#include <Arduino.h>

// Les polices couvrent le Latin-1 : remplace sur place les caractères typographiques courants qui
// n'y sont pas (apostrophe courbe, guillemets anglais, tirets longs, oe lié...)
void toLatin1(char *s);

extern const char *const DAY_NAMES[7];     // "dimanche"...
extern const char *const MONTH_NAMES[12];  // "janvier"...
