// Nombre de personnes dans l'espace en ce moment, lu sur une liste publique tenue à jour à la
// main (HTTPS, sans clé). S'active dans le back office (web.h). Nécessite netBegin().
#pragma once
#include <Arduino.h>

// Relit le réglage gardé en flash et lance la lecture en arrière-plan, puis toutes les 6 heures
void spaceBegin();

// Nombre de personnes, ou -1 s'il n'y a rien à montrer : désactivé, ou pas encore lu
int spacePeople();

bool spaceEnabled();
void spaceSetEnabled(bool on);  // gardé en flash
