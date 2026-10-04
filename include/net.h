// Wi-Fi et heure réseau (NTP). Identifiants dans include/secrets.h, non versionné.
#pragma once
#include <Arduino.h>

// Lance la connexion en arrière-plan, sans bloquer. La reconnexion est automatique.
void netBegin();
bool netConnected();

// Vrai une fois l'heure reçue du réseau. time() et localtime_r() donnent alors l'heure locale.
bool netTimeSynced();
