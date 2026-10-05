// Agendas : les prochains événements de calendriers au format iCalendar, lus en HTTPS.
// Deux listes : celle du Liège Hackerspace (https://lghs.be/calendar.php), et celle des agendas
// personnels dont l'adresse est donnée dans le back office (web.h), fondus en une seule.
// Nécessite netBegin().
#pragma once
#include <Arduino.h>
#include "ical.h"

#define AGENDA_MAX   10  // événements par liste ; les agendas personnels en montrent dix, sur deux colonnes
#define AGENDA_URLS  3  // agendas personnels

enum AgendaList : uint8_t { AGENDA_LGHS, AGENDA_MINE, AGENDA_LISTS };

struct Agenda {
  uint8_t count;
  IcalEvent events[AGENDA_MAX];  // dans l'ordre ; titres déjà au format des polices
  uint8_t source[AGENDA_MAX];    // pour un agenda personnel, son rang : de 0 à AGENDA_URLS - 1
};

// Relit les adresses gardées en flash et lance la lecture en arrière-plan, puis toutes les heures
void agendaBegin();

// Version d'une liste, qui change quand elle change. 0 s'il n'y a rien à montrer : calendrier
// pas encore lu, trop ancien, ou sans événement à venir.
uint32_t agendaVersion(AgendaList which);
void agendaGet(AgendaList which, Agenda &out);

// Secondes écoulées depuis la dernière lecture réussie d'un calendrier de la liste, ou -1
int32_t agendaAge(AgendaList which);

// Ce qui a empêché la dernière lecture d'un calendrier d'aboutir, ou une chaîne vide. Pour le
// back office : c'est le seul endroit où un mauvais lien se voit.
const char *agendaProblem(AgendaList which);

// Adresses des agendas personnels, de 0 à AGENDA_URLS - 1. Une adresse peut être secrète (elle
// donne alors accès à tout l'agenda) : elle est gardée en flash et n'est jamais redonnée.
bool agendaHasUrl(uint8_t index);

// Nom donné à un agenda personnel, affiché avec ses événements. Vide : pas de nom.
#define AGENDA_NAME_SIZE  24
void agendaName(uint8_t index, char *out, size_t cap);
void agendaSetName(uint8_t index, const char *name);  // gardé en flash
void agendaSetUrl(uint8_t index, const char *url);  // "https://...", "http://..." ou "webcal://..." ; vide : la retirer
