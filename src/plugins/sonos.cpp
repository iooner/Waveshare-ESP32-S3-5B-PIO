// Page Sonos : pièce, titre, artiste, album et progression du morceau en cours (sonos.h)
#include "fonts/font_sans24.h"
#include "fonts/font_sans32.h"
#include "fonts/font_sans48.h"
#include "plugin.h"
#include "sonos.h"

#define ROOM_Y      150
#define TITLE_Y     196
#define TITLE_H     67  // hauteur de ligne de la plus grande police du titre
#define ARTIST_Y    282
#define ALBUM_Y     338
#define BAR_Y       440
#define BAR_H       6
#define TIMES_Y     458
#define TIME_W      120
#define COLOR_TRACK  RGB565(60, 60, 60)  // partie non écoulée de la barre

static SonosTrack shown;       // ce qui est à l'écran
static int32_t shown_pos;      // secondes écoulées affichées
static int16_t shown_fill;     // largeur remplie de la barre

static void formatTime(uint32_t seconds, char *out, size_t cap) {
  snprintf(out, cap, "%lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}

// Le titre prend la plus grande police dans laquelle il tient en entier ; sinon la plus petite, coupée
static void drawTitle(const char *title) {
  const GfxFont *font = &font_sans24;
  for (const GfxFont *f : {&font_sans48, &font_sans32}) {
    if (gfxTextWidth(title, *f) <= CONTENT_W) {
      font = f;
      break;
    }
  }
  gfxFillRect(MARGIN_X, TITLE_Y, CONTENT_W, TITLE_H, COLOR_BG);
  gfxTextBox(MARGIN_X, TITLE_Y + (TITLE_H - font->line_height) / 2, CONTENT_W, title, *font, COLOR_TEXT, COLOR_BG);
}

static void drawTrack(const SonosTrack &t) {
  if (t.state == SONOS_NONE) {
    gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);
    gfxTextBox(MARGIN_X, ARTIST_Y, CONTENT_W, "Aucune lecture en cours", font_sans32, COLOR_DIM, COLOR_BG, GFX_CENTER);
    return;
  }
  if (shown.state == SONOS_NONE) gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);

  char room[48];
  snprintf(room, sizeof(room), t.state == SONOS_PAUSED ? "%s · en pause" : "%s", t.room);
  gfxTextBox(MARGIN_X, ROOM_Y, CONTENT_W, room, font_sans24, COLOR_DIM, COLOR_BG);
  drawTitle(t.title);
  gfxTextBox(MARGIN_X, ARTIST_Y, CONTENT_W, t.artist, font_sans32, COLOR_TEXT, COLOR_BG);
  gfxTextBox(MARGIN_X, ALBUM_Y, CONTENT_W, t.album, font_sans24, COLOR_DIM, COLOR_BG);

  char total[12] = "";
  if (t.duration) formatTime(t.duration, total, sizeof(total));
  gfxTextBox(LCD_WIDTH - MARGIN_X - TIME_W, TIMES_Y, TIME_W, total, font_sans24, COLOR_DIM, COLOR_BG, GFX_RIGHT);
}

static void drawProgress(uint32_t pos, uint16_t duration) {
  char elapsed[12];
  formatTime(pos, elapsed, sizeof(elapsed));
  gfxTextBox(MARGIN_X, TIMES_Y, TIME_W, elapsed, font_sans24, COLOR_DIM, COLOR_BG);

  int16_t fill = duration ? (uint32_t)CONTENT_W * pos / duration : 0;
  if (fill == shown_fill) return;
  gfxFillRect(MARGIN_X, BAR_Y, fill, BAR_H, COLOR_TEXT);
  gfxFillRect(MARGIN_X + fill, BAR_Y, CONTENT_W - fill, BAR_H, COLOR_TRACK);
  shown_fill = fill;
}

static void sonosUpdate() {
  SonosTrack t;
  sonosGet(t);
  if (t.state != shown.state || t.duration != shown.duration || strcmp(t.title, shown.title) != 0 ||
      strcmp(t.artist, shown.artist) != 0 || strcmp(t.album, shown.album) != 0 || strcmp(t.room, shown.room) != 0) {
    drawTrack(t);
    shown = t;
    shown_pos = shown_fill = -1;
  }
  if (t.state == SONOS_NONE) return;

  // Entre deux lectures sur l'enceinte, la position avance toute seule
  uint32_t pos = t.position;
  if (t.state == SONOS_PLAYING) pos += (millis() - t.position_at) / 1000;
  if (t.duration && pos > t.duration) pos = t.duration;
  if ((int32_t)pos != shown_pos) {
    drawProgress(pos, t.duration);
    shown_pos = pos;
  }
}

static void sonosShow() {
  gfxFillRect(0, PAGE_Y, LCD_WIDTH, PAGE_HEIGHT, COLOR_BG);
  shown = {};
  shown.title[0] = 1;  // différent de tout état réel : force le dessin
  sonosUpdate();
}

extern const Plugin sonos_plugin = {"sonos", 20, sonosBegin, sonosShow, sonosUpdate};
