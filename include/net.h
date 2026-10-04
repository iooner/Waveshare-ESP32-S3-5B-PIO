// Wi-Fi et heure réseau (NTP). Identifiants dans include/secrets.h, non versionné.
#pragma once
#include <Arduino.h>

// Lance la connexion en arrière-plan, sans bloquer. La reconnexion est automatique.
void netBegin();
bool netConnected();

// Vrai une fois l'heure reçue du réseau. time() et localtime_r() donnent alors l'heure locale.
bool netTimeSynced();

// Fuseau horaire de l'heure locale. Au départ, c'est le fuseau par défaut ; la météo le règle
// ensuite sur celui du lieu choisi (weather.h), et le choix est gardé en flash.
#define NET_TIMEZONE_NAME     "Europe/Brussels"  // nom IANA du fuseau par défaut
#define NET_TIMEZONE_DEFAULT  INT32_MIN

// `utc_offset` : décalage fixe, en secondes à l'est d'UTC, ou NET_TIMEZONE_DEFAULT pour le fuseau
// par défaut, le seul dont la carte connaît les dates de passage à l'heure d'été.
void netSetTimezone(int32_t utc_offset);

// A appeler depuis la boucle principale : enregistre en flash un fuseau qui vient de changer
void netLoop();
