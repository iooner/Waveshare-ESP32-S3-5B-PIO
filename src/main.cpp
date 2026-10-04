#include <Arduino.h>
#include "astro.h"
#include "board.h"
#include "brightness.h"
#include "lcd.h"
#include "net.h"
#include "plugin.h"
#include "web.h"

// Pages par ordre de priorité : la première qui a quelque chose à montrer est affichée.
// Elles s'activent et se désactivent dans le back office (web.h) ; la mire de test est désactivée au départ.
static const Plugin *const pages[] = {&demo_plugin, &sonos_plugin, &clock_plugin};

void setup() {
  Serial.begin(115200);
  // Sans ça, chaque Serial.print bloque plusieurs secondes quand personne ne lit le port USB
  Serial.setTxTimeoutMs(0);
  delay(500);
  Serial.println("Waveshare ESP32-S3-LCD-5B");

  i2cBegin();
  if (!exioBegin()) Serial.println("CH422G introuvable sur l'I2C");

  // Rétroéclairage éteint tant que la première image n'est pas prête
  exioWrite(EXIO_SD_CS, HIGH);
  exioWrite(EXIO_LCD_BL, LOW);
  if (!lcdBegin()) Serial.println("Echec init LCD");

  netBegin();
  astroBegin();
  pluginsBegin(&clock_bar_plugin, pages, sizeof(pages) / sizeof(pages[0]));
  lcdPresent();
  webBegin();

  backlightBegin();
  brightnessBegin();
}

// Toutes les 10 s sur le port série : l'écran a-t-il raté des images ?
static void reportLcdHealth() {
  static uint32_t last = 0;
  if (millis() - last < 10000) return;
  last = millis();
  LcdStats s;
  lcdStats(s);
  Serial.printf("Ecran : %lu images, %lu ratées, %lu morceaux en retard, copie max %lu us, %u lignes lues en PSRAM, "
                "%u cases libres, %lu Ko de RAM interne libres\n",
                (unsigned long)s.frames, (unsigned long)s.bad_frames, (unsigned long)s.late_chunks,
                (unsigned long)s.max_copy_us, s.psram_rows, s.free_slots,
                (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

void loop() {
  // Mise à jour du firmware en cours : les écritures en flash figent le balayage par à-coups,
  // ce qui ne se voit pas sur un écran noir
  if (webUpdating()) {
    static bool cleared = false;
    if (!cleared) gfxClear(COLOR_BG);
    cleared = true;
    lcdPresent();
    return;
  }
  pluginsLoop();
  reportLcdHealth();
  brightnessLoop();
  netLoop();
  // Affiche l'image et attend le rafraîchissement suivant de la dalle
  lcdPresent();
}
