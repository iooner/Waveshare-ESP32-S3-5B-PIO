#include "ical.h"
#include <stdlib.h>
#include <string.h>

#define DAY           86400
#define HORIZON_DAYS  400   // on ne cherche pas d'occurrence plus loin
#define MAX_EXDATES   48
#define MAX_OVERRIDES 64

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
  t += digits(v + 9, 2) * 3600 + digits(v + 11, 2) * 60 + digits(v + 13, 2);
  return v[15] == 'Z' ? utcToLocal(t) : t;
}

// Fin de la ligne courante
static const char *eol(const char *p) {
  while (*p && *p != '\n' && *p != '\r') p++;
  return p;
}

static const char *nextLine(const char *p) {
  p = eol(p);
  while (*p == '\n' || *p == '\r') p++;
  return p;
}

// La ligne commence-t-elle par cette propriété ? Renvoie sa valeur (après les deux-points), ou nul.
static const char *property(const char *line, const char *name) {
  size_t n = strlen(name);
  if (strncmp(line, name, n) != 0 || (line[n] != ':' && line[n] != ';')) return nullptr;
  const char *end = eol(line), *colon = line + n;
  while (colon < end && *colon != ':') colon++;
  return colon < end ? colon + 1 : nullptr;
}

static uint32_t hash(const char *s, const char *end) {
  uint32_t h = 2166136261u;
  for (; s < end; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

// Valeur d'un paramètre de règle, « FREQ=WEEKLY;BYDAY=WE » : ce qui suit « name= », ou nul
static const char *ruleValue(const char *rule, const char *end, const char *name) {
  size_t n = strlen(name);
  for (const char *p = rule; p + n < end; p++) {
    if ((p == rule || p[-1] == ';') && strncmp(p, name, n) == 0 && p[n] == '=') return p + n + 1;
  }
  return nullptr;
}

void icalUnfold(char *text) {
  char *w = text;
  for (const char *r = text; *r;) {
    if (r[0] == '\r' && r[1] == '\n' && (r[2] == ' ' || r[2] == '\t')) r += 3;
    else if (r[0] == '\n' && (r[1] == ' ' || r[1] == '\t')) r += 2;
    else *w++ = *r++;
  }
  *w = 0;
}

// --- Evénements ---

struct Override {
  uint32_t uid;
  time_t local;  // occurrence remplacée
};

struct Rule {
  const char *summary;  // nul : événement sans titre
  const char *rrule, *rrule_end;
  time_t start, end;    // heure locale
  bool all_day;
  bool is_override;
  uint32_t uid;
  time_t exdates[MAX_EXDATES];
  uint8_t exdate_count;
};

// Ajoute un événement aux prochains, gardés triés ; ceux qui sont finis sont écartés
static void add(const Rule &r, time_t local, time_t now, IcalEvent *out, uint8_t cap, uint8_t &count) {
  time_t start = localToUtc(local), end = localToUtc(local + (r.end - r.start));
  if (end <= now) return;
  uint8_t at = count;
  while (at > 0 && out[at - 1].start > start) at--;
  if (at >= cap) return;
  if (count < cap) count++;
  memmove(&out[at + 1], &out[at], (count - at - 1) * sizeof(IcalEvent));
  IcalEvent &e = out[at];
  e.start = start;
  e.end = end;
  e.all_day = r.all_day;
  // Titre : jusqu'à la fin de la ligne, sans les échappements (« \, » « \; » « \n »)
  size_t n = 0;
  for (const char *p = r.summary, *stop = r.summary ? eol(r.summary) : nullptr; p && p < stop && n < sizeof(e.title) - 4; p++) {
    if (*p == '\\' && p + 1 < stop) {
      p++;
      e.title[n++] = *p == 'n' || *p == 'N' ? ' ' : *p;
    } else {
      e.title[n++] = *p;
    }
  }
  while (n > 0 && ((uint8_t)e.title[n - 1] & 0xC0) == 0x80) n--;  // ne pas couper un caractère UTF-8...
  if (n > 0 && (uint8_t)e.title[n - 1] >= 0xC0 && r.summary && eol(r.summary) - r.summary > (long)n) n--;  // ...ni garder son seul début
  e.title[n] = 0;
}

static bool skipped(const Rule &r, time_t local, const Override *overrides, uint8_t override_count) {
  for (uint8_t i = 0; i < r.exdate_count; i++) {
    if (r.exdates[i] == local) return true;
  }
  for (uint8_t i = 0; i < override_count; i++) {
    if (overrides[i].uid == r.uid && overrides[i].local == local) return true;
  }
  return false;
}

// Déroule une récurrence, en heure locale : chaque occurrence garde l'heure de la première
static void expand(const Rule &r, time_t now, const Override *overrides, uint8_t override_count, IcalEvent *out, uint8_t cap,
                   uint8_t &count) {
  const char *freq = ruleValue(r.rrule, r.rrule_end, "FREQ"), *v;
  if (!freq) return;
  int32_t interval = (v = ruleValue(r.rrule, r.rrule_end, "INTERVAL")) ? atoi(v) : 1;
  int32_t left = (v = ruleValue(r.rrule, r.rrule_end, "COUNT")) ? atoi(v) : INT32_MAX;
  time_t until = (v = ruleValue(r.rrule, r.rrule_end, "UNTIL")) ? parseLocal(v) : -1;
  if (interval < 1) interval = 1;

  // BYDAY : jours de la semaine, un bit chacun, et rang du premier (« 1TH », « -1SA »)
  static const char DAYS[] = "SUMOTUWETHFRSA";
  uint8_t weekdays = 0;
  int32_t rank = 0;
  if ((v = ruleValue(r.rrule, r.rrule_end, "BYDAY"))) {
    while (v < r.rrule_end && *v != ';') {
      int32_t n = strtol(v, (char **)&v, 10);
      if (!weekdays) rank = n;
      for (int d = 0; d < 7; d++) {
        if (v + 1 < r.rrule_end && v[0] == DAYS[2 * d] && v[1] == DAYS[2 * d + 1]) weekdays |= 1 << d;
      }
      while (v < r.rrule_end && *v != ',' && *v != ';') v++;
      if (v < r.rrule_end && *v == ',') v++;
    }
  }
  int32_t month_day = (v = ruleValue(r.rrule, r.rrule_end, "BYMONTHDAY")) ? atoi(v) : 0;

  int32_t first = r.start / DAY, last = now / DAY + HORIZON_DAYS, time_of_day = r.start % DAY;
  int32_t y0, m0, d0;
  civilFromDays(first, y0, m0, d0);
  uint8_t found = 0;  // occurrences à venir déjà trouvées pour cette règle : inutile d'en chercher plus que `cap`

  auto occurrence = [&](int32_t day) {  // faux : la récurrence est finie
    time_t local = (time_t)day * DAY + time_of_day;
    if (day > last || (until >= 0 && local > until) || left <= 0) return false;
    left--;
    if (skipped(r, local, overrides, override_count)) return true;
    if (localToUtc(local + (r.end - r.start)) > now) {
      add(r, local, now, out, cap, count);
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
      if (weekdays && freq[0] == 'M') {
        // Rang du jour dans le mois : le 1er jeudi, ou le dernier samedi (rang négatif)
        int32_t wd = __builtin_ctz(weekdays), month = daysFromCivil(y, m, 1), len = daysInMonth(y, m);
        if (rank >= 0) day = month + (wd - weekday(month) + 7) % 7 + 7 * ((rank ? rank : 1) - 1);
        else day = month + len - 1 - (weekday(month + len - 1) - wd + 7) % 7 + 7 * (rank + 1);
        if (day < month || day >= month + len) continue;
      } else {
        int32_t d = month_day > 0 ? month_day : d0;
        if (d > daysInMonth(y, m)) {
          if (daysFromCivil(y, m, 1) > last) break;
          continue;
        }
        day = daysFromCivil(y, m, d);
      }
      if (day < first) continue;
      if (!occurrence(day)) break;
    }
  }
}

uint8_t icalUpcoming(const char *text, time_t now, IcalEvent *out, uint8_t cap) {
  // Premier passage : les occurrences modifiées à la main, qui remplacent celles de leur récurrence.
  // Ces tables tiennent sur la pile de l'appelant (~1,5 Ko).
  Override overrides[MAX_OVERRIDES];
  uint8_t override_count = 0;
  uint32_t uid = 0;
  time_t replaced = -1;
  for (const char *line = text; *line; line = nextLine(line)) {
    const char *v;
    if (strncmp(line, "BEGIN:VEVENT", 12) == 0) uid = 0, replaced = -1;
    else if ((v = property(line, "UID"))) uid = hash(v, eol(v));
    else if ((v = property(line, "RECURRENCE-ID"))) replaced = parseLocal(v);
    else if (strncmp(line, "END:VEVENT", 10) == 0 && replaced >= 0 && override_count < MAX_OVERRIDES) {
      overrides[override_count++] = {uid, replaced};
    }
  }

  // Second passage : chaque événement, déroulé s'il se répète
  Rule r;
  uint8_t count = 0;
  bool inside = false, cancelled = false;
  for (const char *line = text; *line; line = nextLine(line)) {
    const char *v;
    if (strncmp(line, "BEGIN:VEVENT", 12) == 0) {
      r = {};
      r.start = r.end = -1;
      inside = true;
      cancelled = false;
    } else if (!inside) {
      continue;
    } else if ((v = property(line, "DTSTART"))) {
      r.start = parseLocal(v, &r.all_day);
    } else if ((v = property(line, "DTEND"))) {
      r.end = parseLocal(v);
    } else if ((v = property(line, "SUMMARY"))) {
      r.summary = v;
    } else if ((v = property(line, "RRULE"))) {
      r.rrule = v;
      r.rrule_end = eol(v);
    } else if ((v = property(line, "UID"))) {
      r.uid = hash(v, eol(v));
    } else if (property(line, "RECURRENCE-ID")) {
      r.is_override = true;
    } else if ((v = property(line, "STATUS"))) {
      cancelled = strncmp(v, "CANCELLED", 9) == 0;
    } else if ((v = property(line, "EXDATE"))) {
      for (const char *end = eol(v); v < end && r.exdate_count < MAX_EXDATES;) {  // plusieurs dates par ligne, séparées par des virgules
        time_t t = parseLocal(v);
        if (t >= 0) r.exdates[r.exdate_count++] = t;
        while (v < end && *v != ',') v++;
        if (v < end) v++;
      }
    } else if (strncmp(line, "END:VEVENT", 10) == 0) {
      inside = false;
      if (cancelled || r.start < 0) continue;
      if (r.end < r.start) r.end = r.start + (r.all_day ? DAY : 3600);
      if (r.rrule && !r.is_override) expand(r, now, overrides, override_count, out, cap, count);
      else add(r, r.start, now, out, cap, count);
    }
  }
  return count;
}
