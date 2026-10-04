// Météo d'un lieu : temps actuel et prévisions heure par heure, lus chez Open-Meteo (ni compte
// ni clé). Le lieu se règle dans le back office (web.h) ; au départ, c'est celui de
// include/secrets.h. La réponse donne aussi le fuseau horaire du lieu : l'heure locale de la
// carte le suit (net.h). Nécessite netBegin().
#pragma once
#include <Arduino.h>

#define WEATHER_HOURS     12  // heures de prévision gardées, heure en cours comprise
#define WEATHER_QUARTERS  12  // quarts d'heure de prévision de pluie, celui en cours compris

struct WeatherPoint {
  time_t time;   // début de l'heure concernée ; 0 pour le temps actuel
  int8_t temp;   // °C
  uint8_t code;  // code météo WMO : 0 ciel dégagé, 3 couvert, 61 pluie, 95 orage...
  uint8_t rain;  // probabilité de précipitations, en %
  bool day;      // il fait jour
};

struct Weather {
  WeatherPoint now;
  WeatherPoint hours[WEATHER_HOURS];  // heures qui se suivent ; la première est l'heure en cours à la lecture
  uint8_t hour_count;
  // Pluie quart d'heure par quart d'heure, pour annoncer une averse : les heures ne donnent
  // qu'une probabilité
  time_t quarters_from;    // début du premier quart d'heure
  uint16_t rain_quarters;  // bit i : il pleut pendant le quart d'heure i
  uint8_t quarter_count;
};

// Lance la lecture en arrière-plan, puis toutes les 15 minutes
void weatherBegin();

// Dernière météo reçue. Renvoie sa version, qui change à chaque lecture, ou 0 s'il n'y a rien
// à montrer : météo désactivée, rien reçu encore, ou une dernière lecture trop ancienne.
uint32_t weatherGet(Weather &out);

// Secondes écoulées depuis la dernière lecture réussie, ou -1 s'il n'y en a pas eu
int32_t weatherAge();

struct WeatherSettings {
  bool enabled;               // météo affichée ; désactivée, elle reste lue, pour le fuseau horaire
  float latitude, longitude;  // degrés décimaux
};

void weatherSettings(WeatherSettings &out);

// Applique les réglages, les garde en flash et relit la météo sans attendre
void weatherConfigure(const WeatherSettings &s);
