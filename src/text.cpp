#include "text.h"

const char *const DAY_NAMES[7] = {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"};
const char *const MONTH_NAMES[12] = {"janvier", "février", "mars",      "avril",   "mai",      "juin",
                                     "juillet", "août",    "septembre", "octobre", "novembre", "décembre"};

void toLatin1(char *s) {
  static const struct {
    const char *from, *to;  // `to` jamais plus long que `from`
  } MAP[] = {{"‘", "'"}, {"’", "'"}, {"“", "\""}, {"”", "\""}, {"–", "-"},
             {"—", "-"}, {"…", "..."}, {"œ", "oe"}, {"Œ", "OE"}};
  char *w = s;
  for (char *r = s; *r;) {
    bool done = false;
    if ((uint8_t)*r >= 0xC5) {
      for (const auto &m : MAP) {
        size_t n = strlen(m.from);
        if (strncmp(r, m.from, n) == 0) {
          size_t k = strlen(m.to);
          memmove(w, m.to, k);
          w += k;
          r += n;
          done = true;
          break;
        }
      }
    }
    if (!done) *w++ = *r++;
  }
  *w = 0;
}
