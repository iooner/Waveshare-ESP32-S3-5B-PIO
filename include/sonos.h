// Ce que jouent les enceintes Sonos du réseau local, lu directement sur les enceintes
// (UPnP, port 1400) : ni compte ni cloud. Nécessite netBegin().
#pragma once
#include <Arduino.h>

enum SonosState : uint8_t { SONOS_NONE, SONOS_PAUSED, SONOS_PLAYING };

struct SonosTrack {
  SonosState state;      // SONOS_NONE : rien à afficher, les autres champs sont vides
  char room[32];
  char title[128];
  char artist[96];
  char album[96];
  uint16_t duration;     // secondes, 0 si inconnue (radio)
  uint16_t position;     // secondes écoulées au moment de la lecture
  uint32_t position_at;  // millis() de cette lecture
};

// Lance la recherche des enceintes et leur interrogation en arrière-plan
void sonosBegin();

// Dernier état connu : l'enceinte qui joue, à défaut une enceinte en pause
void sonosGet(SonosTrack &out);

// Pochette du morceau en cours : SONOS_ART_SIZE x SONOS_ART_SIZE pixels RGB565, ou nul s'il n'y
// en a pas. `version` change chaque fois que la pochette change.
#define SONOS_ART_SIZE  300
const uint16_t *sonosArt(uint32_t &version);
