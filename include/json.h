// Lecture de réponses JSON simples, sans analyseur : on cherche une clé à partir d'un point du
// texte. Suffit pour des objets dont on connaît la forme.
#pragma once
#include <Arduino.h>

// Ce qui suit `"key":` après `from`, ou nul
static inline const char *jsonValue(const char *from, const char *key) {
  char pattern[64];
  int len = snprintf(pattern, sizeof(pattern), "\"%s\":", key);
  const char *p = from ? strstr(from, pattern) : nullptr;
  return p ? p + len : nullptr;
}
