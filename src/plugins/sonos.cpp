// Page Sonos : pochette, pièce, titre, artiste, album et progression du morceau en cours (sonos.h)
#include "fonts/font_sans32.h"
#include "fonts/font_sans40.h"
#include "fonts/font_sans48.h"
#include "plugin.h"
#include "sonos.h"

#define ART_X       MARGIN_X
#define ART_Y       130
#define ART_STEP    20  // lignes de pochette dessinées par image : elle se déroule en ~0,4 s
#define TEXT_X      (ART_X + SONOS_ART_SIZE + 40)
#define TEXT_W      (LCD_WIDTH - MARGIN_X - TEXT_X)
#define ROOM_Y      128
#define TITLE_Y     186
#define TITLE_H     67  // hauteur de ligne de la plus grande police du titre
#define ARTIST_Y    274
#define ALBUM_Y     340
#define BAR_Y       474
#define BAR_H       6
#define TIMES_Y     492
#define TIME_W      160
#define COLOR_TRACK  RGB565(60, 60, 60)  // partie non écoulée de la barre, pochette absente

static SonosTrack shown;        // ce qui est à l'écran
static int32_t shown_pos;       // secondes écoulées affichées
static int16_t shown_fill;      // largeur remplie de la barre
static uint32_t shown_art;      // version de la pochette affichée ou en cours de dessin
static const uint16_t *art;     // pochette en cours de dessin, nul = emplacement vide
static int16_t art_row;         // prochaine ligne à dessiner ; SONOS_ART_SIZE = terminé

static void formatTime(uint32_t seconds, char *out, size_t cap) {
  snprintf(out, cap, "%lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
}

// Le titre prend la plus grande police dans laquelle il tient en entier ; sinon la plus petite, coupée
static void drawTitle(const char *title) {
  const GfxFont *font = &font_sans32;
  for (const GfxFont *f : {&font_sans48, &font_sans40}) {
    if (gfxTextWidth(title, *f) <= TEXT_W) {
      font = f;
      break;
    }
  }
  gfxFillRect(TEXT_X, TITLE_Y, TEXT_W, TITLE_H, COLOR_BG);
  gfxTextBox(TEXT_X, TITLE_Y + (TITLE_H - font->line_height) / 2, TEXT_W, title, *font, COLOR_TEXT, COLOR_BG);
}

static void drawTrack(const SonosTrack &t) {
  gfxTextBox(TEXT_X, ROOM_Y, TEXT_W, t.room, font_sans32, COLOR_DIM, COLOR_BG);
  drawTitle(t.title);
  gfxTextBox(TEXT_X, ARTIST_Y, TEXT_W, t.artist, font_sans40, COLOR_TEXT, COLOR_BG);
  gfxTextBox(TEXT_X, ALBUM_Y, TEXT_W, t.album, font_sans32, COLOR_DIM, COLOR_BG);

  char total[12] = "";
  if (t.duration) formatTime(t.duration, total, sizeof(total));
  gfxTextBox(LCD_WIDTH - MARGIN_X - TIME_W, TIMES_Y, TIME_W, total, font_sans32, COLOR_DIM, COLOR_BG, GFX_RIGHT);
}

static void drawProgress(uint32_t pos, uint16_t duration) {
  char elapsed[12];
  formatTime(pos, elapsed, sizeof(elapsed));
  gfxTextBox(MARGIN_X, TIMES_Y, TIME_W, elapsed, font_sans32, COLOR_DIM, COLOR_BG);

  int16_t fill = duration ? (uint32_t)CONTENT_W * pos / duration : 0;
  if (fill == shown_fill) return;
  gfxFillRect(MARGIN_X, BAR_Y, fill, BAR_H, COLOR_TEXT);
  gfxFillRect(MARGIN_X + fill, BAR_Y, CONTENT_W - fill, BAR_H, COLOR_TRACK);
  shown_fill = fill;
}

// La pochette est dessinée par tranches, une par image : écrire 180 Ko d'un coup en PSRAM
// pendant qu'une image y est lue pour l'écran pourrait faire rater une image
static void drawArtSlice() {
  uint32_t version;
  const uint16_t *latest = sonosArt(version);
  if (version != shown_art) shown_art = version, art = latest, art_row = 0;
  if (art_row >= SONOS_ART_SIZE) return;

  // Dans le noir d'un fondu, le balayage ne lit pas la PSRAM : la pochette peut être écrite d'un coup
  int16_t n = lcdFade() == 0 ? SONOS_ART_SIZE - art_row : min<int16_t>(ART_STEP, SONOS_ART_SIZE - art_row);
  if (art) gfxBlit(ART_X, ART_Y + art_row, SONOS_ART_SIZE, n, art + art_row * SONOS_ART_SIZE);
  else gfxFillRect(ART_X, ART_Y + art_row, SONOS_ART_SIZE, n, COLOR_TRACK);
  art_row += n;
}

static bool sonosActive() {
  SonosTrack t;
  sonosGet(t);
  return t.state == SONOS_PLAYING;
}

static void sonosUpdate() {
  SonosTrack t;
  sonosGet(t);
  if (t.state != SONOS_PLAYING) return;  // la page va céder la place
  if (t.duration != shown.duration || strcmp(t.title, shown.title) != 0 || strcmp(t.artist, shown.artist) != 0 ||
      strcmp(t.album, shown.album) != 0 || strcmp(t.room, shown.room) != 0) {
    drawTrack(t);
    shown = t;
    shown_pos = shown_fill = -1;
  }
  drawArtSlice();

  // Entre deux lectures sur l'enceinte, la position avance toute seule
  uint32_t pos = t.position + (millis() - t.position_at) / 1000;
  if (t.duration && pos > t.duration) pos = t.duration;
  if ((int32_t)pos != shown_pos) {
    drawProgress(pos, t.duration);
    shown_pos = pos;
  }
}

static void sonosShow() {
  lcdImageArea(ART_X, ART_Y, SONOS_ART_SIZE, SONOS_ART_SIZE);
  shown = {};
  shown.title[0] = 1;  // différent de tout titre réel : force le dessin
  sonosArt(shown_art);
  shown_art--;         // idem pour la pochette
  sonosUpdate();
}

// La pochette est effacée par tranches avant de rendre l'écran, pour la même raison qu'à l'affichage
static void sonosHide() {
  for (int16_t y = 0; y < SONOS_ART_SIZE && lcdFade() != 0; y += ART_STEP) {
    gfxFillRect(ART_X, ART_Y + y, SONOS_ART_SIZE, ART_STEP, COLOR_BG);
    lcdPresent();
  }
  lcdImageArea(0, 0, 0, 0);
  lcdPresent();
}

extern const Plugin sonos_plugin = {"sonos", false, sonosBegin, sonosActive, sonosShow, sonosUpdate, sonosHide, false, true};
