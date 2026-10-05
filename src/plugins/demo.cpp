// Plugin de démonstration : mire de test (couleurs, texte), carré animé et compteur de FPS.
// Sert aussi d'exemple d'animation : on ne repeint que ce qui change d'une image à l'autre.
#include "fonts/font_sans24.h"
#include "fonts/font_sans48.h"
#include "gfx.h"
#include "lcd.h"
#include "plugin.h"

#define BALL_SIZE   60
#define BALL_Y      120
#define BALL_SPEED  400  // px/s

// Compteur d'images par seconde : zone fixe, plus large que le texte le plus long
#define FPS_Y  520
#define FPS_W  320

static int16_t ball_x;
static uint32_t frames, last_report;

static void textCentered(int16_t y, const char *text, const GfxFont &font) {
  gfxText((LCD_WIDTH - gfxTextWidth(text, font)) / 2, y, text, font, COLOR_TEXT, COLOR_BG);
}

static void demoShow() {
  textCentered(240, "Hello World!", font_sans48);
  textCentered(320, "Écran prêt, 1024 x 600", font_sans24);
  // Barres de test : ordre attendu rouge, vert, bleu
  gfxFillRect(362, 400, 100, 60, COLOR_RED);
  gfxFillRect(462, 400, 100, 60, COLOR_GREEN);
  gfxFillRect(562, 400, 100, 60, COLOR_BLUE);

  gfxFillRect(0, BALL_Y, LCD_WIDTH, BALL_SIZE, COLOR_BG);
  ball_x = -BALL_SIZE;  // hors écran : rien à effacer à la première image
  frames = 0;
  last_report = millis();
}

static void demoUpdate() {
  // Position calculée sur le temps, donc vitesse indépendante des FPS
  uint32_t span = LCD_WIDTH - BALL_SIZE;
  uint32_t pos = (uint64_t)millis() * BALL_SPEED / 1000 % (2 * span);
  int16_t x = pos < span ? pos : 2 * span - pos;
  int16_t dx = x - ball_x;
  if (dx != 0) {
    // La nouvelle position, puis seulement la bande que le carré vient de quitter
    gfxFillRect(x, BALL_Y, BALL_SIZE, BALL_SIZE, COLOR_BLUE);
    if (abs(dx) >= BALL_SIZE) gfxFillRect(ball_x, BALL_Y, BALL_SIZE, BALL_SIZE, COLOR_BG);
    else if (dx > 0) gfxFillRect(ball_x, BALL_Y, dx, BALL_SIZE, COLOR_BG);
    else gfxFillRect(x + BALL_SIZE, BALL_Y, -dx, BALL_SIZE, COLOR_BG);
    ball_x = x;
  }

  frames++;
  uint32_t now = millis();
  if (now - last_report >= 1000) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f FPS", frames * 1000.0f / (now - last_report));
    Serial.println(buf);
    gfxFillRect((LCD_WIDTH - FPS_W) / 2, FPS_Y, FPS_W, font_sans24.line_height, COLOR_BG);
    textCentered(FPS_Y, buf, font_sans24);
    frames = 0;
    last_report = now;
  }
}

extern const Plugin demo_plugin = {"mire", false, nullptr, nullptr, demoShow, demoUpdate, nullptr, true, true};
