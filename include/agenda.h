// Agenda du Liège Hackerspace : ses prochains événements, lus dans son calendrier public
// (https://lghs.be/calendar.php, au format iCalendar). Nécessite netBegin().
#pragma once
#include <Arduino.h>
#include "ical.h"

#define AGENDA_MAX  5

struct Agenda {
  uint8_t count;
  IcalEvent events[AGENDA_MAX];  // dans l'ordre ; titres déjà au format des polices
};

// Lance la lecture du calendrier en arrière-plan, puis toutes les heures
void agendaBegin();

// Version de l'agenda, qui change quand la liste change. 0 s'il n'y a rien à montrer : calendrier
// pas encore lu, trop ancien, ou sans événement à venir.
uint32_t agendaVersion();
void agendaGet(Agenda &out);

// Secondes écoulées depuis la dernière lecture réussie du calendrier, ou -1 s'il n'y en a pas eu
int32_t agendaAge();
