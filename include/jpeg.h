// Décodage d'images JPEG avec le décodeur de la ROM de l'ESP32-S3 (TJpgDec) : JPEG de base
// seulement, pas les JPEG progressifs.
#pragma once
#include <Arduino.h>

// Décode `data` en RGB565 dans `out`, réduit pour tenir dans max_w x max_h en gardant ses
// proportions, jamais agrandi. `w` et `h` reçoivent sa taille : `out` en contient h lignes de w
// pixels, et doit pouvoir en contenir max_w x max_h. Faux si l'image est illisible.
// Plusieurs tâches peuvent s'en servir : elles passent une à la fois.
bool jpegDecode(const uint8_t *data, size_t len, uint16_t *out, uint16_t max_w, uint16_t max_h, uint16_t &w, uint16_t &h);
