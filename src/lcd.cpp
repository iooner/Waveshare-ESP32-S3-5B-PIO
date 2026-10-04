#include "lcd.h"
#include <esp32s3/rom/cache.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include "board.h"

// La PSRAM est trop lente pour alimenter le LCD en direct : l'image part à l'écran par morceaux,
// depuis deux petits tampons en RAM interne (bounce buffers) que l'on recopie à tour de rôle
// depuis une interruption. La taille d'un morceau doit diviser l'image en un nombre pair.
#define LCD_BOUNCE_LINES  10
#define LCD_BOUNCE_PX     (LCD_WIDTH * LCD_BOUNCE_LINES)
#define LCD_CHUNKS        (LCD_HEIGHT / LCD_BOUNCE_LINES)
#define LCD_FB_PX         (LCD_WIDTH * LCD_HEIGHT)

// Grille de temps du balayage, en cycles CPU (240 MHz) : sert à mesurer les retards
#define CPU_MHZ       240
#define LINE_CYCLES   ((uint32_t)(CPU_MHZ * 1000000ULL * (LCD_WIDTH + LCD_HPW + LCD_HBP + LCD_HFP) / LCD_PCLK_HZ))
#define CHUNK_CYCLES  (LINE_CYCLES * LCD_BOUNCE_LINES)
#define FRAME_CYCLES  (LINE_CYCLES * (LCD_HEIGHT + LCD_VPW + LCD_VBP + LCD_VFP))

// Nombre de zones modifiées suivies par image. Au-delà, elles sont fusionnées entre elles.
#define LCD_MAX_DIRTY  16

struct Rect {
  int16_t x, y, w, h;
};

uint16_t *lcd_fb;

static esp_lcd_panel_handle_t panel;
static uint16_t *fbs[2];
static uint8_t *bounce[2];  // les deux tampons du pilote, relevés à ses deux premiers appels
static volatile uint8_t scan = 0;     // framebuffer lu par le balayage
static volatile uint8_t pending = 0;  // celui demandé pour l'image suivante
static uint8_t back = 1;
static SemaphoreHandle_t frame_sem;
static Rect dirty[LCD_MAX_DIRTY];
static uint8_t dirty_count = 0;

// --- Lignes servies depuis la RAM interne ---
// Lire la PSRAM pendant le balayage est ce qui fait rater des images dès que le réseau travaille :
// le bus est partagé avec la flash, et la copie d'un morceau n'arrive plus à temps. Mesuré Wi-Fi
// allumé : 1 à 6 images ratées par seconde en lisant la PSRAM, aucune sans y toucher.
// Chaque ligne affichée est donc décrite ici, en RAM interne : une couleur de fond, et entre x0
// et x1 son contenu, gardé sous forme compacte (16 couleurs au plus, 4 bits par pixel) quand
// c'est possible. Sinon cette partie est lue en PSRAM, comme une image.
#define ROW_SLOTS      224   // lignes compactes disponibles (544 octets chacune)
#define NO_SLOT        0xFFFF
#define RAW_NARROW_PX  128   // contenu assez étroit pour être lu en PSRAM sans risque

struct RowInfo {
  uint16_t bg;      // couleur de la ligne en dehors de [x0, x1)
  uint16_t x0, x1;  // pairs ; x0 == x1 : ligne unie
  uint16_t slot;    // ligne compacte, ou NO_SLOT si [x0, x1) est lu dans le framebuffer
};

struct PackedRow {
  uint16_t palette[16];
  uint8_t px[LCD_WIDTH / 2];  // deux pixels par octet, à partir de x0
};

struct RowUpdate {
  uint16_t y;
  RowInfo info;
};

static RowInfo rows[LCD_HEIGHT];           // décrit le framebuffer affiché ; modifié seulement entre deux images
static PackedRow *pool;
static uint16_t free_slots[ROW_SLOTS], free_count;
static RowUpdate staged[LCD_HEIGHT];       // lignes de la prochaine image, appliquées à l'échange
static volatile uint16_t staged_count = 0;
static portMUX_TYPE swap_mux = portMUX_INITIALIZER_UNLOCKED;

static LcdStats stats;
static portMUX_TYPE stats_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t grid_base;  // instant de référence de la grille pour l'image en cours
static bool grid_set = false;
static uint8_t last_chunk = 1;

// Remplit n pixels (n pair, dst aligné sur 4 octets)
static inline __attribute__((always_inline)) void fillPairs(uint16_t *dst, uint16_t color, uint32_t n) {
  uint32_t c32 = (uint32_t)color << 16 | color;
  uint32_t *d = (uint32_t *)dst;
  for (n >>= 1; n; n--) *d++ = c32;
}

// Développe n pixels d'une ligne compacte (n pair, dst aligné sur 4 octets)
static inline __attribute__((always_inline)) void unpack(uint16_t *dst, const PackedRow &row, uint32_t n) {
  const uint8_t *src = row.px;
  const uint16_t *pal = row.palette;
  uint32_t *d = (uint32_t *)dst;
  for (n >>= 1; n; n--) {
    uint8_t b = *src++;
    *d++ = pal[b >> 4] | (uint32_t)pal[b & 15] << 16;
  }
}

// Décrit une ligne du framebuffer de dessin. Prend une ligne compacte dans la réserve si le
// contenu est large, tient en 16 couleurs, et qu'il en reste une.
static void describeRow(const uint16_t *src, RowInfo &r) {
  uint16_t bg = src[0];
  int x0 = 0, x1 = LCD_WIDTH;
  while (x0 < LCD_WIDTH && src[x0] == bg) x0++;
  r.bg = bg;
  r.slot = NO_SLOT;
  if (x0 == LCD_WIDTH) {
    r.x0 = r.x1 = 0;
    return;
  }
  while (src[x1 - 1] == bg) x1--;
  r.x0 = x0 &= ~1;
  r.x1 = x1 = (x1 + 1) & ~1;
  if (x1 - x0 <= RAW_NARROW_PX || free_count == 0) return;

  PackedRow &p = pool[free_slots[free_count - 1]];
  p.palette[0] = bg;
  uint8_t colors = 1, last_index = 0;
  uint16_t last = bg;
  uint8_t *dst = p.px;
  for (int x = x0; x < x1; x += 2) {
    uint8_t pair = 0;
    for (int k = 0; k < 2; k++) {
      uint16_t c = src[x + k];
      if (c != last) {
        uint8_t i = 0;
        while (i < colors && p.palette[i] != c) i++;
        if (i == colors) {
          if (colors == 16) return;  // trop de couleurs : la ligne sera lue en PSRAM
          p.palette[colors++] = c;
        }
        last = c;
        last_index = i;
      }
      pair = pair << 4 | last_index;
    }
    *dst++ = pair;
  }
  r.slot = free_slots[--free_count];
}

// Le pilote appelle cette fonction quand un tampon vient de partir à l'écran, pour le remplir
// avec le morceau suivant. Le pilote n'a pas de framebuffer à lui (mode no_fb) : c'est nous qui
// choisissons la source, ce qui permet de changer de framebuffer pile entre deux images et de
// mesurer chaque copie.
static bool IRAM_ATTR onBounceEmpty(esp_lcd_panel_handle_t, void *buf, int pos_px, int len_bytes, void *) {
  uint32_t t0 = esp_cpu_get_cycle_count();
  uint32_t chunk = pos_px / LCD_BOUNCE_PX;

  // Le pilote choisit le tampon à remplir en comptant les interruptions (bb_eof_count % 2 dans
  // lcd_rgb_panel_eof_handler), et ce compteur n'est jamais remis à zéro quand
  // CONFIG_LCD_RGB_RESTART_IN_VSYNC est actif, ce qui est le cas ici. S'il perd une interruption,
  // il remplit ensuite le tampon en cours d'envoi : l'image reste décalée de 10 lignes jusqu'à
  // la perte suivante. Or le balayage repart du premier tampon à chaque image : les morceaux
  // pairs vont donc toujours dans le premier, les impairs dans le second. On s'y tient.
  uint8_t *dst = bounce[chunk & 1];
  if (!dst) dst = bounce[chunk & 1] = (uint8_t *)buf;  // deux premiers appels, dans lcdBegin()
  if (dst != buf) stats.fixed_chunks++;

  const uint16_t *fb = fbs[scan];
  uint16_t *out = (uint16_t *)dst;
  uint32_t y = chunk * LCD_BOUNCE_LINES;
  for (uint32_t i = 0; i < LCD_BOUNCE_LINES; i++, y++, out += LCD_WIDTH) {
    const RowInfo &r = rows[y];
    fillPairs(out, r.bg, r.x0);
    fillPairs(out + r.x1, r.bg, LCD_WIDTH - r.x1);
    if (r.x0 == r.x1) continue;
    if (r.slot != NO_SLOT) unpack(out + r.x0, pool[r.slot], r.x1 - r.x0);
    else memcpy(out + r.x0, fb + y * LCD_WIDTH + r.x0, (r.x1 - r.x0) * sizeof(uint16_t));
  }

  // Si le morceau suivant est surtout lu en PSRAM (une image), on le précharge dans le cache
  // pendant que celui-ci part à l'écran : sa copie est alors 6 fois plus rapide
  uint32_t next = chunk + 1 < LCD_CHUNKS ? chunk + 1 : 0;
  const RowInfo *nr = &rows[next * LCD_BOUNCE_LINES];
  uint32_t raw_px = 0;
  for (uint32_t i = 0; i < LCD_BOUNCE_LINES; i++) {
    if (nr[i].slot == NO_SLOT) raw_px += nr[i].x1 - nr[i].x0;
  }
  if (raw_px > LCD_BOUNCE_PX / 2) {
    const uint16_t *next_fb = next ? fb : fbs[pending];
    Cache_Start_DCache_Preload((uint32_t)(next_fb + next * LCD_BOUNCE_PX), len_bytes, 0);
  }
  uint32_t dt = esp_cpu_get_cycle_count() - t0;

  // Retard de cette copie sur la grille du balayage. Les morceaux 0 et 1 d'une image sont
  // copiés à la fin de l'image précédente, d'où le décalage de 2.
  uint32_t slot = chunk >= 2 ? chunk - 1 : chunk + LCD_CHUNKS - 1;
  if (!grid_set) grid_base = t0 - slot * CHUNK_CYCLES, grid_set = true;
  int32_t late = (int32_t)(t0 - grid_base) - (int32_t)(slot * CHUNK_CYCLES);
  while (late > (int32_t)(FRAME_CYCLES / 2)) grid_base += FRAME_CYCLES, late -= FRAME_CYCLES;
  if (late < 0) grid_base += late, late = 0;  // la grille se cale sur la copie la plus en avance

  // Le tampon repart à l'écran un morceau plus tard : la copie doit avoir commencé avant,
  // et finir avant que l'envoi ne la rattrape
  if ((uint32_t)late > CHUNK_CYCLES || late + dt > 2 * CHUNK_CYCLES) stats.late_chunks++;
  if (dt > stats.max_copy_us) stats.max_copy_us = dt;  // en cycles, converti à la lecture
  last_chunk = chunk;
  return false;
}

static bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  stats.frames++;
  // A la synchro verticale, les deux premiers morceaux de l'image suivante doivent être prêts.
  // Sinon une interruption a été perdue et la fin de l'image est partie décalée.
  if (last_chunk != 1) stats.bad_frames++;
  return false;
}

// Appelé quand la dernière ligne d'une image a été copiée : le changement de framebuffer se
// fait ici, donc jamais au milieu d'une image
static bool IRAM_ATTR onFrameDone(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  portENTER_CRITICAL_ISR(&swap_mux);
  for (uint32_t i = 0, n = staged_count; i < n; i++) rows[staged[i].y] = staged[i].info;
  staged_count = 0;
  scan = pending;
  portEXIT_CRITICAL_ISR(&swap_mux);
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(frame_sem, &woken);
  return woken == pdTRUE;
}

void lcdStats(LcdStats &out) {
  portENTER_CRITICAL(&stats_mux);
  out = stats;
  stats = {};
  portEXIT_CRITICAL(&stats_mux);
  out.max_copy_us /= CPU_MHZ;
  for (const RowInfo &r : rows) {
    if (r.slot == NO_SLOT && r.x1 - r.x0 > RAW_NARROW_PX) out.psram_rows++;
  }
  out.free_slots = free_count;
}

bool lcdBegin() {
  exioWrite(EXIO_LCD_RST, LOW);
  delay(20);
  exioWrite(EXIO_LCD_RST, HIGH);
  delay(120);

  for (uint8_t i = 0; i < 2; i++) {
    fbs[i] = (uint16_t *)heap_caps_aligned_calloc(64, 1, LCD_FB_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!fbs[i]) return false;
  }
  // Framebuffers à zéro : toutes les lignes sont unies et noires
  for (RowInfo &r : rows) r = {0, 0, 0, NO_SLOT};
  pool = (PackedRow *)heap_caps_malloc(ROW_SLOTS * sizeof(PackedRow), MALLOC_CAP_INTERNAL);
  if (!pool) return false;
  for (free_count = 0; free_count < ROW_SLOTS; free_count++) free_slots[free_count] = free_count;

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = LCD_PCLK_HZ;
  cfg.timings.h_res = LCD_WIDTH;
  cfg.timings.v_res = LCD_HEIGHT;
  cfg.timings.hsync_pulse_width = LCD_HPW;
  cfg.timings.hsync_back_porch = LCD_HBP;
  cfg.timings.hsync_front_porch = LCD_HFP;
  cfg.timings.vsync_pulse_width = LCD_VPW;
  cfg.timings.vsync_back_porch = LCD_VBP;
  cfg.timings.vsync_front_porch = LCD_VFP;
  cfg.timings.flags.hsync_idle_low = 1;
  cfg.timings.flags.vsync_idle_low = 1;
  cfg.timings.flags.pclk_active_neg = LCD_PCLK_ACTIVE_NEG;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.bounce_buffer_size_px = LCD_BOUNCE_PX;
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = LCD_PIN_HSYNC;
  cfg.vsync_gpio_num = LCD_PIN_VSYNC;
  cfg.de_gpio_num = LCD_PIN_DE;
  cfg.pclk_gpio_num = LCD_PIN_PCLK;
  cfg.disp_gpio_num = -1;
  const int data_pins[16] = {
      LCD_PIN_B3, LCD_PIN_B4, LCD_PIN_B5, LCD_PIN_B6, LCD_PIN_B7,
      LCD_PIN_G2, LCD_PIN_G3, LCD_PIN_G4, LCD_PIN_G5, LCD_PIN_G6, LCD_PIN_G7,
      LCD_PIN_R3, LCD_PIN_R4, LCD_PIN_R5, LCD_PIN_R6, LCD_PIN_R7};
  memcpy(cfg.data_gpio_nums, data_pins, sizeof(data_pins));
  cfg.flags.no_fb = 1;

  frame_sem = xSemaphoreCreateBinary();
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = onVsync;
  cbs.on_bounce_empty = onBounceEmpty;
  cbs.on_frame_buf_complete = onFrameDone;

  // Les interruptions du LCD sont allouées sur le coeur appelant : coeur 1 depuis setup()
  if (esp_lcd_new_rgb_panel(&cfg, &panel) != ESP_OK) return false;
  if (esp_lcd_rgb_panel_register_event_callbacks(panel, &cbs, nullptr) != ESP_OK) return false;
  if (esp_lcd_panel_reset(panel) != ESP_OK) return false;
  if (esp_lcd_panel_init(panel) != ESP_OK) return false;

  lcd_fb = fbs[back];
  return true;
}

static inline bool contains(const Rect &a, const Rect &b) {
  return b.x >= a.x && b.y >= a.y && b.x + b.w <= a.x + a.w && b.y + b.h <= a.y + a.h;
}

static inline Rect unite(const Rect &a, const Rect &b) {
  int16_t x0 = min(a.x, b.x), y0 = min(a.y, b.y);
  int16_t x1 = max<int16_t>(a.x + a.w, b.x + b.w), y1 = max<int16_t>(a.y + a.h, b.y + b.h);
  return {x0, y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0)};
}

void lcdDirty(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!lcdClip(x, y, w, h)) return;
  Rect n = {x, y, w, h};

  for (uint8_t i = 0; i < dirty_count; i++) {
    if (contains(dirty[i], n)) return;
    if (contains(n, dirty[i])) {
      dirty[i] = n;
      return;
    }
  }
  if (dirty_count < LCD_MAX_DIRTY) {
    dirty[dirty_count++] = n;
    return;
  }
  // Liste pleine : on agrandit la zone pour laquelle ça ajoute le moins de pixels à recopier
  uint8_t best = 0;
  int32_t best_cost = INT32_MAX;
  for (uint8_t i = 0; i < dirty_count; i++) {
    Rect u = unite(dirty[i], n);
    int32_t cost = (int32_t)u.w * u.h - (int32_t)dirty[i].w * dirty[i].h;
    if (cost < best_cost) best_cost = cost, best = i;
  }
  dirty[best] = unite(dirty[best], n);
}

void lcdPresent() {
  xSemaphoreTake(frame_sem, 0);
  if (dirty_count == 0) {
    xSemaphoreTake(frame_sem, pdMS_TO_TICKS(200));
    return;
  }

  // Décrit les lignes modifiées, telles qu'elles sont dans le framebuffer de dessin
  static uint32_t touched[(LCD_HEIGHT + 31) / 32];
  static uint16_t old_slots[LCD_HEIGHT];
  memset(touched, 0, sizeof(touched));
  for (uint8_t i = 0; i < dirty_count; i++) {
    for (int16_t y = dirty[i].y; y < dirty[i].y + dirty[i].h; y++) touched[y >> 5] |= 1u << (y & 31);
  }
  uint16_t n = 0;
  for (uint16_t y = 0; y < LCD_HEIGHT; y++) {
    if (!(touched[y >> 5] & 1u << (y & 31))) continue;
    staged[n].y = y;
    describeRow(lcd_fb + y * LCD_WIDTH, staged[n].info);
    old_slots[n++] = rows[y].slot;
  }

  // Le balayage passe sur le nouveau framebuffer et ses lignes à la fin de l'image en cours.
  // Les deux demandes partent ensemble : l'interruption ne doit pas voir l'une sans l'autre.
  portENTER_CRITICAL(&swap_mux);
  pending = back;
  staged_count = n;
  portEXIT_CRITICAL(&swap_mux);
  bool swapped = true;
  while (scan != back) {
    if (!xSemaphoreTake(frame_sem, pdMS_TO_TICKS(200))) {  // LCD arrêté : on ne reste pas bloqué
      swapped = false;
      staged_count = 0;
      break;
    }
  }
  // Les lignes compactes qui ne servent plus retournent dans la réserve
  for (uint16_t i = 0; i < n; i++) {
    uint16_t unused = swapped ? old_slots[i] : staged[i].info.slot;
    if (unused != NO_SLOT) free_slots[free_count++] = unused;
  }

  // L'ancien framebuffer affiché devient celui de dessin : on y reporte ce qui vient d'être
  // dessiné dans l'autre, pour que les deux restent identiques.
  const uint16_t *front = fbs[back];
  back ^= 1;
  lcd_fb = fbs[back];
  for (uint8_t i = 0; i < dirty_count; i++) {
    const Rect &r = dirty[i];
    for (int16_t y = r.y; y < r.y + r.h; y++) {
      size_t off = (size_t)y * LCD_WIDTH + r.x;
      memcpy(lcd_fb + off, front + off, r.w * sizeof(uint16_t));
    }
  }
  dirty_count = 0;
}
