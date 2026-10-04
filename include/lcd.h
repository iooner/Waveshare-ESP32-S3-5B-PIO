// Pilote LCD RGB : esp_lcd en direct, deux framebuffers en PSRAM, échange synchronisé sur l'image.
// Le dessin et lcdPresent() doivent rester dans une seule tâche.
#pragma once
#include <Arduino.h>
#include "board_pins.h"

// Reset de la dalle puis démarrage du balayage. Nécessite i2cBegin() et exioBegin().
bool lcdBegin();

// Framebuffer de dessin : LCD_WIDTH x LCD_HEIGHT pixels RGB565, jamais celui qui est à l'écran.
// Le pointeur change à chaque lcdPresent(), le contenu est conservé d'une image à l'autre.
extern uint16_t *lcd_fb;

// A appeler pour chaque zone modifiée directement dans lcd_fb (les fonctions gfx* le font déjà)
void lcdDirty(int16_t x, int16_t y, int16_t w, int16_t h);

// Affiche lcd_fb à la prochaine image et bloque jusque-là : cadence la boucle sur l'écran.
// Sans zone modifiée, attend simplement l'image suivante.
void lcdPresent();

// Santé du balayage depuis la lecture précédente. `bad_frames` et `late_chunks` doivent rester
// à zéro : une image ratée se voit à l'écran (bande périmée ou décalée pendant une image).
struct LcdStats {
  uint32_t frames;        // images balayées
  uint32_t bad_frames;    // images dont un morceau a été perdu
  uint32_t late_chunks;   // morceaux prêts trop tard : des lignes périmées sont parties à l'écran
  uint32_t fixed_chunks;  // morceaux que le pilote envoyait dans le mauvais tampon, remis dans le bon
  uint32_t max_copy_us;   // plus longue copie d'un morceau (limite : la durée d'un morceau, ~420 us)
};
void lcdStats(LcdStats &out);

// Découpe un rectangle aux bords de l'écran. Faux s'il n'en reste rien.
static inline bool lcdClip(int16_t &x, int16_t &y, int16_t &w, int16_t &h) {
  if (x < 0) w += x, x = 0;
  if (y < 0) h += y, y = 0;
  if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
  if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
  return w > 0 && h > 0;
}
