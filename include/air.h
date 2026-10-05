// Particules fines mesurées par un capteur du réseau local sous firmware Sensor.Community
// (http://<capteur>/data.json). L'adresse du capteur se donne dans le back office (web.h).
// Nécessite netBegin().
#pragma once
#include <Arduino.h>
#include "gfx.h"

struct AirSettings {
  bool enabled;
  char host[64];  // adresse ou nom du capteur sur le réseau local
  bool bar;       // le niveau en toutes lettres dans la barre du haut des autres pages
};

// Relit les réglages gardés en flash et lance la lecture en arrière-plan, puis toutes les 3 minutes
void airBegin();

// Dernière mesure, en µg/m³. Faux s'il n'y a rien à montrer : désactivé, pas d'adresse, ou
// capteur muet depuis trop longtemps.
bool airGet(float &pm25, float &pm10);

// Les dix niveaux de l'indice belge de qualité de l'air (BelAQI), du meilleur au pire
#define AIR_LEVELS  10
struct AirLevel {
  const char *name;
  uint16_t color;
};
extern const AirLevel AIR_LEVEL[AIR_LEVELS];

// Niveau d'une mesure en µg/m³, de 0 à AIR_LEVELS - 1
uint8_t airLevelPm25(float value);
uint8_t airLevelPm10(float value);

// Niveau à montrer dans la barre du haut : la moyenne des deux, arrondie vers le pire. Faux s'il
// n'y a rien à montrer ou si le réglage n'est pas coché.
bool airBarLevel(uint8_t &level);

void airSettings(AirSettings &out);
void airConfigure(const AirSettings &s);  // gardé en flash
