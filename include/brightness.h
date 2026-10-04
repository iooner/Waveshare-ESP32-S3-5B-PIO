// Luminosité de l'écran : fixe, ou en cycle automatique. Le cycle suit le soleil au lieu réglé
// pour la météo : luminosité de jour quand il est levé, de nuit quand il est couché, en fondu
// pendant l'aube et le crépuscule. En option, les couleurs suivent le même fondu vers un rouge
// sombre la nuit (gfx.h). L'écran peut aussi s'éteindre tout à fait entre deux heures choisies,
// et la carte avec lui : en veille profonde, l'ESP32 s'arrête et son horloge le réveille à
// l'heure du rallumage. Le back office ne répond plus pendant ce temps ; un appui sur le bouton
// reset, ou une remise sous tension, la garde éveillée quelques minutes pour pouvoir changer le
// réglage. Se règle dans le back office (web.h).
// Nécessite backlightBegin(), weatherBegin() et netBegin().
#pragma once
#include <Arduino.h>

struct BrightnessSettings {
  uint8_t day;     // pour cent, de 1 à 100. Sans cycle automatique, c'est la seule utilisée.
  uint8_t night;   // pour cent, de 0 (écran éteint) à 100
  bool automatic;  // cycle automatique
  bool red;        // couleurs en rouge sombre la nuit
  bool sleep;      // écran éteint de sleep_from à sleep_to, chaque jour
  uint16_t sleep_from, sleep_to;  // en minutes depuis minuit, heure locale
  bool deep;       // pendant l'extinction, la carte passe en veille profonde
};

// Relit les réglages gardés en flash et allume l'écran
void brightnessBegin();

// A appeler à chaque image : applique les réglages et fait avancer le cycle
void brightnessLoop();

void brightnessSettings(BrightnessSettings &out);

// Luminosité en place, en pour cent
uint8_t brightnessCurrent();

// Applique les réglages et les garde en flash
void brightnessConfigure(const BrightnessSettings &s);
