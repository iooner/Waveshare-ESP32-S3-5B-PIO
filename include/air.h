// Particules fines mesurées par un capteur du réseau local sous firmware Sensor.Community
// (http://<capteur>/data.json). L'adresse du capteur se donne dans le back office (web.h).
// Nécessite netBegin().
#pragma once
#include <Arduino.h>

struct AirSettings {
  bool enabled;
  char host[64];  // adresse ou nom du capteur sur le réseau local
};

// Relit les réglages gardés en flash et lance la lecture en arrière-plan, puis toutes les 3 minutes
void airBegin();

// Dernière mesure, en µg/m³. Faux s'il n'y a rien à montrer : désactivé, pas d'adresse, ou
// capteur muet depuis trop longtemps.
bool airGet(float &pm25, float &pm10);

void airSettings(AirSettings &out);
void airConfigure(const AirSettings &s);  // gardé en flash
