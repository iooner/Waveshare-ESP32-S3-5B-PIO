// Soleil et lune, calculés sur la carte à partir de l'heure et du lieu, sans réseau : hauteur du
// soleil, heures de lever et de coucher, phase de la lune. Formules à l'ordre le plus bas, justes
// à deux minutes près pour le soleil sous nos latitudes et à une heure près pour la lune.
//
// Ce module garde aussi le choix d'afficher ou non le soleil et la lune, réglé dans le back
// office (web.h).
#pragma once
#include <Arduino.h>

// Hauteur du soleil au-dessus de l'horizon, en degrés
float sunElevation(time_t t, float latitude, float longitude);

enum SunDay : uint8_t { SUN_RISES, SUN_ALWAYS_UP, SUN_ALWAYS_DOWN };

// Lever et coucher du soleil le jour dont `noon` est le milieu. Autre résultat que SUN_RISES :
// jour ou nuit polaire, `rise` et `set` ne sont pas remplis.
SunDay sunTimes(time_t noon, float latitude, float longitude, time_t &rise, time_t &set);

// Phase de la lune, en huitièmes de lunaison : 0 nouvelle lune, 2 premier quartier, 4 pleine
// lune, 6 dernier quartier
uint8_t moonPhase(time_t t);

struct AstroSettings {
  bool sun;   // lever et coucher du soleil affichés
  bool moon;  // phase de la lune affichée
};

// Relit les réglages gardés en flash
void astroBegin();
void astroSettings(AstroSettings &out);

// Applique les réglages et les garde en flash
void astroConfigure(const AstroSettings &s);
