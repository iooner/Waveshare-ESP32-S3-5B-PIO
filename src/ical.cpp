#include "ical.h"
#include <stdlib.h>
#include <string.h>

#define DAY           86400
#define HORIZON_DAYS  400  // on ne cherche pas d'occurrence plus loin

// --- Dates ---

// Jours écoulés depuis le 1er janvier 1970 (algorithme de Howard Hinnant)
static int32_t daysFromCivil(int32_t y, int32_t m, int32_t d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  int32_t yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static void civilFromDays(int32_t z, int32_t &y, int32_t &m, int32_t &d) {
  z += 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
  int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int32_t mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = yoe + era * 400 + (m <= 2);
}

// 0 = dimanche
static int32_t weekday(int32_t days) {
  return ((days + 4) % 7 + 7) % 7;
}

static int32_t daysInMonth(int32_t y, int32_t m) {
  return daysFromCivil(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - daysFromCivil(y, m, 1);
}

// Heure d'été à Bruxelles : du dernier dimanche de mars au dernier dimanche d'octobre, à 1 h UTC
static bool summerTime(time_t utc) {
  int32_t y, m, d;
  civilFromDays(utc / DAY, y, m, d);
  int32_t march = daysFromCivil(y, 3, 31), october = daysFromCivil(y, 10, 31);
  time_t from = (time_t)(march - weekday(march)) * DAY + 3600, to = (time_t)(october - weekday(october)) * DAY + 3600;
  return utc >= from && utc < to;
}

// Heure locale de Bruxelles, donnée en secondes comme si elle était universelle, vers le temps universel
static time_t localToUtc(time_t local) {
  return summerTime(local - 7200) ? local - 7200 : local - 3600;
}

static time_t utcToLocal(time_t utc) {
  return utc + (summerTime(utc) ? 7200 : 3600);
}

// --- Lecture des lignes ---

static int32_t digits(const char *s, int n) {
  int32_t v = 0;
  while (n--) v = v * 10 + (*s++ - '0');
  return v;
}

// Une date du calendrier, « 20261007T160000 », suivie de Z si elle est universelle, ou
// « 20261007 » pour une journée entière. Renvoie l'heure LOCALE, en secondes ; -1 si mal formée.
static time_t parseLocal(const char *v, bool *all_day = nullptr) {
  for (int i = 0; i < 8; i++) {
    if (v[i] < '0' || v[i] > '9') return -1;
  }
  time_t t = (time_t)daysFromCivil(digits(v, 4), digits(v + 4, 2), digits(v + 6, 2)) * DAY;
  bool timed = v[8] == 'T';
  if (all_day) *all_day = !timed;
  if (!timed) return t;
  for (int i = 9; i < 15; i++) {
    if (v[i] < '0' || v[i] > '9') return -1;
  }
  t += digits(v + 9, 2) * 3600 + digits(v + 11, 2) * 60 + digits(v + 13, 2);
  return v[15] == 'Z' ? utcToLocal(t) : t;
}

// La ligne commence-t-elle par cette propriété ? Renvoie sa valeur (après les deux-points), ou nul.
static const char *property(const char *line, const char *name) {
  size_t n = strlen(name);
  if (strncmp(line, name, n) != 0 || (line[n] != ':' && line[n] != ';')) return nullptr;
  const char *colon = strchr(line + n, ':');
  return colon ? colon + 1 : nullptr;
}

static uint32_t hash(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

// Valeur d'un paramètre de règle, « FREQ=WEEKLY;BYDAY=WE » : ce qui suit « name= », ou nul
static const char *ruleValue(const char *rule, const char *name) {
  size_t n = strlen(name);
  for (const char *p = rule; *p; p++) {
    if ((p == rule || p[-1] == ';') && strncmp(p, name, n) == 0 && p[n] == '=') return p + n + 1;
  }
  return nullptr;
}

// Copie un titre sans les échappements du format (« \, » « \; » « \n »), sans couper un caractère UTF-8
static void copyTitle(char *out, size_t cap, const char *text) {
  size_t n = 0;
  const char *p = text;
  for (; *p && n < cap - 4; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      out[n++] = *p == 'n' || *p == 'N' ? ' ' : *p;
    } else {
      out[n++] = *p;
    }
  }
  if (*p) {  // coupé : on retire un caractère entamé
    while (n > 0 && ((uint8_t)out[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0 && (uint8_t)out[n - 1] >= 0xC0) n--;
  }
  out[n] = 0;
}

// --- Prochains événements ---

// Ajoute un événement aux prochains, gardés triés ; un événement fini est écarté
static void add(const char *title, time_t start, time_t end, bool all_day, time_t now, IcalEvent *out, uint8_t cap,
                uint8_t &count) {
  if (end <= now) return;
  uint8_t at = count;
  while (at > 0 && out[at - 1].start > start) at--;
  if (at >= cap) return;
  if (count < cap) count++;
  memmove(&out[at + 1], &out[at], (count - at - 1) * sizeof(IcalEvent));
  IcalEvent &e = out[at];
  memset(&e, 0, sizeof(e));
  e.start = start;
  e.end = end;
  e.all_day = all_day;
  strncpy(e.title, title, sizeof(e.title) - 1);
}

static bool skipped(const IcalCalendar &cal, const IcalMaster &r, time_t local) {
  for (uint8_t i = 0; i < r.exdate_count; i++) {
    if (r.exdates[i] == local) return true;
  }
  for (uint16_t i = 0; i < cal.override_count; i++) {
    if (cal.overrides[i].uid == r.uid && cal.overrides[i].local == local) return true;
  }
  return false;
}

// Déroule une récurrence, en heure locale
static void expand(const IcalCalendar &cal, const IcalMaster &r, time_t now, IcalEvent *out, uint8_t cap, uint8_t &count) {
  const char *freq = ruleValue(r.rule, "FREQ"), *v;
  if (!freq) return;
  int32_t interval = (v = ruleValue(r.rule, "INTERVAL")) ? atoi(v) : 1;
  int32_t left = (v = ruleValue(r.rule, "COUNT")) ? atoi(v) : INT32_MAX;
  time_t until = (v = ruleValue(r.rule, "UNTIL")) ? parseLocal(v) : -1;
  if (interval < 1) interval = 1;

  // BYDAY : jours de la semaine, un bit chacun, et rang du premier (« 1TH », « -1SA »)
  static const char DAYS[] = "SUMOTUWETHFRSA";
  uint8_t weekdays = 0;
  int32_t rank = 0;
  if ((v = ruleValue(r.rule, "BYDAY"))) {
    while (*v && *v != ';') {
      int32_t n = strtol(v, (char **)&v, 10);
      if (!weekdays) rank = n;
      for (int d = 0; d < 7; d++) {
        if (v[0] == DAYS[2 * d] && v[1] == DAYS[2 * d + 1]) weekdays |= 1 << d;
      }
      while (*v && *v != ',' && *v != ';') v++;
      if (*v == ',') v++;
    }
  }
  int32_t month_day = (v = ruleValue(r.rule, "BYMONTHDAY")) ? atoi(v) : 0;

  int32_t first = r.start / DAY, last = now / DAY + HORIZON_DAYS, time_of_day = r.start % DAY;
  int32_t y0, m0, d0;
  civilFromDays(first, y0, m0, d0);
  uint8_t found = 0;  // occurrences à venir déjà trouvées pour cette règle : inutile d'en chercher plus que `cap`

  auto occurrence = [&](int32_t day) {  // faux : la récurrence est finie
    time_t local = (time_t)day * DAY + time_of_day;
    if (day > last || (until >= 0 && local > until) || left <= 0) return false;
    left--;
    if (skipped(cal, r, local)) return true;
    time_t end = localToUtc(local + (r.end - r.start));
    if (end > now) {
      add(r.title, localToUtc(local), end, r.all_day, now, out, cap, count);
      if (++found >= cap) return false;
    }
    return true;
  };

  if (freq[0] == 'D') {
    for (int32_t day = first; occurrence(day); day += interval) {}
  } else if (freq[0] == 'W') {
    if (!weekdays) weekdays = 1 << weekday(first);
    int32_t week0 = first - (weekday(first) + 6) % 7;  // lundi de la première semaine
    for (int32_t day = first;; day++) {
      if (!(weekdays >> weekday(day) & 1) || (day - week0) / 7 % interval) {
        if (day > last) break;
        continue;
      }
      if (!occurrence(day)) break;
    }
  } else if (freq[0] == 'M' || freq[0] == 'Y') {
    int32_t step = freq[0] == 'Y' ? 12 * interval : interval;
    for (int32_t k = 0;; k += step) {
      int32_t y = y0 + (m0 - 1 + k) / 12, m = (m0 - 1 + k) % 12 + 1, day;
      if (daysFromCivil(y, m, 1) > last) break;
      if (weekdays && freq[0] == 'M') {
        // Rang du jour dans le mois : le 1er jeudi, ou le dernier samedi (rang négatif)
        int32_t wd = __builtin_ctz(weekdays), month = daysFromCivil(y, m, 1), len = daysInMonth(y, m);
        if (rank >= 0) day = month + (wd - weekday(month) + 7) % 7 + 7 * ((rank ? rank : 1) - 1);
        else day = month + len - 1 - (weekday(month + len - 1) - wd + 7) % 7 + 7 * (rank + 1);
        if (day < month || day >= month + len) continue;
      } else {
        int32_t d = month_day > 0 ? month_day : d0;
        if (d > daysInMonth(y, m)) continue;
        day = daysFromCivil(y, m, d);
      }
      if (day < first) continue;
      if (!occurrence(day)) break;
    }
  }
}

uint8_t icalUpcoming(const IcalCalendar &cal, time_t now, IcalEvent *out, uint8_t cap) {
  uint8_t count = 0;
  for (uint16_t i = 0; i < cal.single_count; i++) {
    const IcalEvent &e = cal.singles[i];
    add(e.title, e.start, e.end, e.all_day, now, out, cap, count);
  }
  for (uint16_t i = 0; i < cal.master_count; i++) expand(cal, cal.masters[i], now, out, cap, count);
  return count;
}

// --- Lecture au fil de l'eau ---

// Range l'événement qui vient de se terminer, s'il peut encore servir
static void finishEvent(IcalCalendar &cal) {
  IcalMaster &e = cal.event;
  if (e.start < 0) return;
  if (e.end < e.start) e.end = e.start + (e.all_day ? DAY : 3600);
  time_t recent = utcToLocal(cal.now) - 2 * DAY;  // heure locale en deçà de laquelle plus rien ne sert

  if (cal.is_override) {
    // Occurrence modifiée à la main : elle remplace celle de sa récurrence, même annulée
    if (cal.replaced >= recent) {
      if (cal.override_count < ICAL_OVERRIDES) cal.overrides[cal.override_count++] = {e.uid, cal.replaced};
      else cal.truncated = true;
    }
  } else if (e.rule[0] && !cal.cancelled) {
    // Une récurrence arrêtée dans le passé ne donnera plus rien : un agenda ancien en est plein
    const char *until = ruleValue(e.rule, "UNTIL");
    if (until && parseLocal(until) >= 0 && parseLocal(until) < recent) return;
    if (cal.master_count < ICAL_MASTERS) cal.masters[cal.master_count++] = e;
    else cal.truncated = true;
    return;
  }
  if (cal.cancelled) return;
  time_t end = localToUtc(e.end);
  if (end <= cal.now) return;
  if (cal.single_count >= ICAL_SINGLES) {
    cal.truncated = true;
    return;
  }
  IcalEvent &single = cal.singles[cal.single_count++];
  memset(&single, 0, sizeof(single));
  single.start = localToUtc(e.start);
  single.end = end;
  single.all_day = e.all_day;
  memcpy(single.title, e.title, sizeof(single.title));
}

// Traite une ligne entière, replis déjà recollés
static void finishLine(IcalCalendar &cal) {
  cal.line[cal.line_len] = 0;
  cal.line_len = 0;
  const char *line = cal.line, *v;
  if (strncmp(line, "END:VCALENDAR", 13) == 0) {
    cal.complete = true;
  } else if (strncmp(line, "BEGIN:VEVENT", 12) == 0) {
    memset(&cal.event, 0, sizeof(cal.event));
    cal.event.start = cal.event.end = -1;
    cal.in_event = true;
    cal.is_override = cal.cancelled = false;
    cal.replaced = -1;
  } else if (!cal.in_event) {
    return;
  } else if ((v = property(line, "DTSTART"))) {
    cal.event.start = parseLocal(v, &cal.event.all_day);
  } else if ((v = property(line, "DTEND"))) {
    cal.event.end = parseLocal(v);
  } else if ((v = property(line, "SUMMARY"))) {
    copyTitle(cal.event.title, sizeof(cal.event.title), v);
  } else if ((v = property(line, "RRULE"))) {
    strncpy(cal.event.rule, v, sizeof(cal.event.rule) - 1);
  } else if ((v = property(line, "UID"))) {
    cal.event.uid = hash(v);
  } else if ((v = property(line, "RECURRENCE-ID"))) {
    cal.is_override = true;
    cal.replaced = parseLocal(v);
  } else if ((v = property(line, "STATUS"))) {
    cal.cancelled = strncmp(v, "CANCELLED", 9) == 0;
  } else if ((v = property(line, "EXDATE"))) {
    // Plusieurs dates par ligne, séparées par des virgules ; les dates passées ne servent plus
    time_t recent = utcToLocal(cal.now) - 2 * DAY;
    while (*v) {
      time_t t = parseLocal(v);
      if (t >= recent) {
        if (cal.event.exdate_count < ICAL_EXDATES) cal.event.exdates[cal.event.exdate_count++] = t;
        else cal.truncated = true;
      }
      while (*v && *v != ',') v++;
      if (*v) v++;
    }
  } else if (strncmp(line, "END:VEVENT", 10) == 0) {
    cal.in_event = false;
    finishEvent(cal);
  }
}

void icalBegin(IcalCalendar &cal, time_t now) {
  cal.complete = cal.truncated = false;
  cal.master_count = cal.single_count = cal.override_count = 0;
  cal.now = now;
  cal.line_len = 0;
  cal.at_eol = cal.in_event = false;
}

void icalFeed(IcalCalendar &cal, const char *data, size_t len) {
  for (; len; len--, data++) {
    char c = *data;
    if (c == '\r') continue;
    if (cal.at_eol) {
      // Une ligne qui commence par un blanc est la suite de la précédente
      cal.at_eol = false;
      if (c == ' ' || c == '\t') continue;
      finishLine(cal);
    }
    if (c == '\n') cal.at_eol = true;
    else if (cal.line_len < ICAL_LINE - 1) cal.line[cal.line_len++] = c;
  }
}

void icalEnd(IcalCalendar &cal) {
  if (cal.at_eol || cal.line_len) finishLine(cal);
  cal.at_eol = false;
}
