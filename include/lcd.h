// Pilote LCD RGB : esp_lcd en direct, deux framebuffers en PSRAM, échange synchronisé sur l'image.
// Le dessin et lcdPresent() doivent rester dans une seule tâche.
#pragma once
#include <Arduino.h>
#include "board_pins.h"

// Reset de la dalle puis démarrage du balayage. Nécessite i2cBegin() et exioBegin().
bool lcdBegin();

// Framebuffer de dessin : LCD_WIDTH x LCD_HEIGHT pixels RGB565, jamais celui qui est à l'écran.
// Le pointeur change à chaque lcdPresent(), le contenu est conservé d'une image à l'autre.
//
// Pour une image qui ne saute jamais, même quand le réseau travaille : pas plus de 16 couleurs
// sur une même ligne de pixels (un fond et un texte d'une seule couleur en font 16 avec
// l'antialiasing). Ces lignes sont affichées depuis la RAM interne. Les autres (photo, dégradé,
// deux textes de couleurs différentes côte à côte) sont lues en PSRAM, plus fragile.
extern uint16_t *lcd_fb;

// Déclare le rectangle de l'écran qui contient une image (photo, pochette) ; w = 0 pour l'enlever.
// Cette partie est lue en PSRAM, et le reste de ses lignes continue d'être servi depuis la RAM
// interne. Mesuré : sans risque jusqu'à 320 pixels de large, des images ratées au-delà de 450.
void lcdImageArea(int16_t x, int16_t y, int16_t w, int16_t h);

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
  uint16_t psram_rows;    // lignes affichées lues en PSRAM : elles seules peuvent rater quand le réseau travaille
  uint16_t free_slots;    // cases de 64 pixels encore disponibles en RAM interne
};
void lcdStats(LcdStats &out);

// Images ratées et morceaux en retard depuis le démarrage, plus longue copie d'un morceau depuis
// la lecture précédente (en microsecondes ; elle doit rester sous deux durées de morceau, ~840)
uint32_t lcdBadFrames();
uint32_t lcdLateChunks();
uint32_t lcdMaxCopyUs();

// Fondu : clarté de tout l'écran, de 0 (noir) à LCD_FADE_MAX (couleurs telles quelles), par
// paliers à peu près réguliers pour l'oeil. Elle est appliquée pendant le balayage, sans rien
// redessiner, et change d'une image à la suivante. A 0, le balayage ne lit plus la PSRAM : c'est
// le moment d'y écrire en grand.
#define LCD_FADE_MAX  9
void lcdSetFade(uint8_t level);
uint8_t lcdFade();  // clarté de l'image en cours de balayage

// Découpe un rectangle aux bords de l'écran. Faux s'il n'en reste rien.
static inline bool lcdClip(int16_t &x, int16_t &y, int16_t &w, int16_t &h) {
  if (x < 0) w += x, x = 0;
  if (y < 0) h += y, y = 0;
  if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
  if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
  return w > 0 && h > 0;
}
