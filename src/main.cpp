#include <Arduino.h>
#include "board.h"
#include "lcd.h"
#include "net.h"
#include "plugin.h"

#define BACKLIGHT_PERCENT  10

// Pages affichées sous la barre, dans l'ordre de défilement. Ajouter &demo_plugin pour la mire de test.
static const Plugin *const pages[] = {&sonos_plugin};

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
  gfxClear(COLOR_BG);
  pluginsBegin(&clock_plugin, pages, sizeof(pages) / sizeof(pages[0]));
  lcdPresent();

  backlightBegin();
  backlightSet(BACKLIGHT_PERCENT);
}

// Toutes les 10 s sur le port série : l'écran a-t-il raté des images ?
static void reportLcdHealth() {
  static uint32_t last = 0;
  if (millis() - last < 10000) return;
  last = millis();
  LcdStats s;
  lcdStats(s);
  Serial.printf("Ecran : %lu images, %lu ratées, %lu morceaux en retard, copie max %lu us, %u lignes lues en PSRAM, "
                "%u lignes compactes libres, %lu Ko de RAM interne libres\n",
                (unsigned long)s.frames, (unsigned long)s.bad_frames, (unsigned long)s.late_chunks,
                (unsigned long)s.max_copy_us, s.psram_rows, s.free_slots,
                (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

void loop() {
  pluginsLoop();
  reportLcdHealth();
  // Affiche l'image et attend le rafraîchissement suivant de la dalle
  lcdPresent();
}
