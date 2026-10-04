#include <Arduino.h>
#include "board.h"
#include "lcd.h"
#include "net.h"
#include "plugin.h"

#define BACKLIGHT_PERCENT  10

// Plugins affichés, dans l'ordre de défilement. Ajouter &demo_plugin pour la mire de test.
static const Plugin *const plugins[] = {&clock_plugin};

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
  pluginsBegin(plugins, sizeof(plugins) / sizeof(plugins[0]));
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
  Serial.printf("Ecran : %lu images, %lu ratées, %lu morceaux en retard, %lu remis dans le bon tampon, copie max %lu us\n",
                (unsigned long)s.frames, (unsigned long)s.bad_frames, (unsigned long)s.late_chunks,
                (unsigned long)s.fixed_chunks, (unsigned long)s.max_copy_us);
}

void loop() {
  pluginsLoop();
  reportLcdHealth();
  // Affiche l'image et attend le rafraîchissement suivant de la dalle
  lcdPresent();
}
