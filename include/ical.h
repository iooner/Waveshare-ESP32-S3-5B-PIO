// Lecture d'un calendrier iCalendar (RFC 5545), juste ce qu'il faut pour un agenda : les
// prochains événements, récurrences comprises. Sans dépendance à Arduino : se teste sur un PC.
//
// Le calendrier est lu au fil de l'eau, morceau par morceau, sans être gardé en entier : seuls
// restent les événements qui se répètent et ceux qui ne sont pas finis. Un agenda de plusieurs
// mégaoctets tient ainsi dans ~160 Ko.
//
// Compris : événements simples, journées entières, récurrences DAILY, WEEKLY (BYDAY), MONTHLY
// (BYDAY avec rang, comme « 1er jeudi » ou « dernier samedi », ou jour du mois) et YEARLY, avec
// INTERVAL, UNTIL, COUNT, EXDATE, et les occurrences modifiées à la main (RECURRENCE-ID).
// Les heures locales du calendrier sont prises pour celles de Bruxelles, quel que soit leur TZID.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define ICAL_MASTERS    256  // événements récurrents gardés
#define ICAL_SINGLES    256  // événements simples à venir gardés
#define ICAL_OVERRIDES  256  // occurrences modifiées à la main gardées
#define ICAL_EXDATES    32   // dates annulées gardées par récurrence
#define ICAL_LINE       400  // début gardé d'une ligne ; les descriptions, plus longues, ne servent pas

struct IcalEvent {
  time_t start;    // début, en temps universel
  time_t end;
  bool all_day;
  char title[80];  // UTF-8, sans les échappements du format
};

// Evénement récurrent, en heure locale : chaque occurrence garde l'heure de la première
struct IcalMaster {
  time_t start, end;
  bool all_day;
  uint32_t uid;  // condensé de son identifiant
  char title[80];
  char rule[128];
  time_t exdates[ICAL_EXDATES];
  uint8_t exdate_count;
};

struct IcalOverride {
  uint32_t uid;
  time_t local;  // occurrence remplacée
};

struct IcalCalendar {
  bool complete;   // la fin du calendrier a été lue
  bool truncated;  // une des listes était pleine : des événements manquent
  uint16_t master_count, single_count, override_count;
  IcalMaster masters[ICAL_MASTERS];
  IcalEvent singles[ICAL_SINGLES];
  IcalOverride overrides[ICAL_OVERRIDES];

  // Lecture en cours
  time_t now;
  char line[ICAL_LINE];
  uint16_t line_len;
  bool at_eol, in_event, is_override, cancelled;
  time_t replaced;
  IcalMaster event;
};

// Lecture : icalBegin(), puis icalFeed() pour chaque morceau reçu, puis icalEnd(). Les événements
// finis à l'instant `now` ne sont pas gardés.
void icalBegin(IcalCalendar &cal, time_t now);
void icalFeed(IcalCalendar &cal, const char *data, size_t len);
void icalEnd(IcalCalendar &cal);

// Les `cap` prochains événements du calendrier, dans l'ordre : ceux qui ne sont pas finis à
// l'instant `now`. Renvoie leur nombre.
uint8_t icalUpcoming(const IcalCalendar &cal, time_t now, IcalEvent *out, uint8_t cap);
