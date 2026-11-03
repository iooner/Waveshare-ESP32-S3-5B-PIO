#include "text.h"

const char *const DAY_NAMES[7] = {"dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi"};
const char *const MONTH_NAMES[12] = {"janvier", "février", "mars",      "avril",   "mai",      "juin",
                                     "juillet", "août",    "septembre", "octobre", "novembre", "décembre"};

const char *const MONTH_SHORT[12] = {"janv.", "févr.", "mars", "avr.", "mai", "juin", "juil.", "août", "sept.", "oct.", "nov.", "déc."};

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

void dropUnsupported(char *s) {
  char *w = s;
  for (const uint8_t *r = (const uint8_t *)s; *r;) {
    // Un caractère UTF-8 : son premier octet dit sa longueur
    uint8_t len = *r >= 0xF0 ? 4 : *r >= 0xE0 ? 3 : *r >= 0xC0 ? 2 : 1;
    uint32_t cp = len == 1 ? *r : *r & (0xFF >> (len + 1));
    uint8_t n = 1;
    while (n < len && (r[n] & 0xC0) == 0x80) cp = cp << 6 | (r[n++] & 0x3F);
    // Les polices ont le Latin-1 et l'euro
    bool drawable = n == len && ((cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF) || cp == 0x20AC);
    bool space = cp == ' ' || cp == 0xA0;
    if (drawable && !(space && (w == s || w[-1] == ' '))) {  // ni espace en tête, ni deux de suite
      if (space) *w++ = ' ';
      else for (uint8_t i = 0; i < n; i++) *w++ = r[i];
    }
    r += n;
  }
  while (w > s && w[-1] == ' ') w--;
  *w = 0;
}

// Montant en euros à la française : "76 658 €", "2 418,87 €", "0,1523 €"
void formatEuros(double amount, char *out, size_t cap) {
  char digits[24];
  int decimals = amount >= 10000 ? 0 : amount >= 1 ? 2 : 4;
  snprintf(digits, sizeof(digits), "%.*f", decimals, amount);
  char *dot = strchr(digits, '.');
  size_t whole = dot ? dot - digits : strlen(digits), n = 0;
  for (size_t i = 0; i < whole && n < cap - 1; i++) {
    if (i && (whole - i) % 3 == 0) out[n++] = ' ';  // espace des milliers
    out[n++] = digits[i];
  }
  out[n] = 0;
  if (dot) snprintf(out + n, cap - n, ",%s", dot + 1);
  strlcat(out, " €", cap);
}
