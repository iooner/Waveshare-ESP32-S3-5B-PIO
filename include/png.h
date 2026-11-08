// Décodage d'images PNG, avec le décompresseur zlib de la ROM de l'ESP32-S3 (miniz) : toutes les
// sortes de PNG, sauf les PNG entrelacés. C'est le format des vignettes que Wikimedia tire des SVG.
#pragma once
#include <Arduino.h>

// Vrai si l'image a des pixels transparents possibles : canal alpha ou couleur transparente
bool pngHasAlpha(const uint8_t *data, size_t len);

// Décode `data` en RGB565 dans `out`, comme jpegDecode() (jpeg.h) : réduite pour tenir dans
// max_w x max_h, jamais agrandie, h lignes de w pixels. Les pixels transparents sont posés sur `bg`.
// Faux si l'image est illisible ou trop grande.
bool pngDecode(const uint8_t *data, size_t len, uint16_t *out, uint16_t max_w, uint16_t max_h, uint16_t &w, uint16_t &h,
               uint16_t bg);
