// Heure réseau (NTP). Identifiants Wi-Fi dans include/secrets.h, non versionné.
// Le Wi-Fi n'est allumé que le temps de recevoir l'heure, une fois par jour : voir src/net.cpp.
#pragma once
#include <Arduino.h>

// Lance la mise à l'heure en arrière-plan, sans bloquer
void netBegin();

// Vrai pendant qu'une mise à l'heure est en cours et que le Wi-Fi est connecté
bool netConnected();

// Vrai une fois l'heure reçue du réseau. time() et localtime_r() donnent alors l'heure locale.
bool netTimeSynced();
