// Lecture d'un calendrier iCalendar (RFC 5545), juste ce qu'il faut pour un agenda : les
// prochains événements, récurrences comprises. Sans dépendance à Arduino : se teste sur un PC.
//
// Compris : événements simples, journées entières, récurrences DAILY, WEEKLY (BYDAY), MONTHLY
// (BYDAY avec rang, comme « 1er jeudi » ou « dernier samedi », ou jour du mois) et YEARLY, avec
// INTERVAL, UNTIL, COUNT, EXDATE, et les occurrences modifiées à la main (RECURRENCE-ID).
// Les heures locales du calendrier sont prises pour celles de Bruxelles, quel que soit leur TZID.
#pragma once
#include <stdint.h>
#include <time.h>

struct IcalEvent {
  time_t start;    // début, en temps universel
  time_t end;
  bool all_day;
  char title[80];  // UTF-8, sans les échappements du format
};

// Retire les replis de lignes du calendrier, sur place. A faire une fois, avant icalUpcoming().
void icalUnfold(char *text);

// Les `cap` prochains événements du calendrier, dans l'ordre : ceux qui ne sont pas finis à
// l'instant `now`. Renvoie leur nombre.
uint8_t icalUpcoming(const char *text, time_t now, IcalEvent *out, uint8_t cap);
