// Fonctions matérielles de la carte : expander CH422G et rétroéclairage
#pragma once
#include <Arduino.h>

// Initialise Wire sur les broches de la carte. A appeler avant tout le reste.
void i2cBegin();

// Expander CH422G. Utilisable depuis plusieurs tâches.
bool exioBegin();
void exioWrite(uint8_t pin, bool level);

// Rétroéclairage gradable : 0 = éteint, 100 = pleine luminosité
void backlightBegin();
void backlightSet(uint8_t percent);
