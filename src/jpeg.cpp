#include "jpeg.h"
#include <esp32s3/rom/tjpgd.h>

#define POOL_SIZE  4096  // mémoire de travail du décodeur

struct Io {
  const uint8_t *data;
  size_t len, pos;
  uint16_t *out;
  uint32_t src_w, src_h;  // taille de l'image après la réduction du décodeur
  uint16_t w, h;          // taille voulue
};

static UINT readData(JDEC *jd, BYTE *buf, UINT n) {
  Io *io = (Io *)jd->device;
  n = min((size_t)n, io->len - io->pos);
  if (buf) memcpy(buf, io->data + io->pos, n);
  io->pos += n;
  return n;
}

// Reçoit un bloc décodé en RGB888 et le range en RGB565. Ce qui reste à réduire après le
// décodeur l'est au plus proche voisin.
static UINT writeBlock(JDEC *jd, void *bitmap, JRECT *r) {
  Io *io = (Io *)jd->device;
  const uint8_t *px = (const uint8_t *)bitmap;
  for (uint32_t y = r->top; y <= r->bottom; y++) {
    uint32_t ty = y * io->h / io->src_h;
    for (uint32_t x = r->left; x <= r->right; x++, px += 3) {
      uint32_t tx = x * io->w / io->src_w;
      if (tx < io->w && ty < io->h) io->out[ty * io->w + tx] = (px[0] & 0xF8) << 8 | (px[1] & 0xFC) << 3 | px[2] >> 3;
    }
  }
  return 1;
}

bool jpegDecode(const uint8_t *data, size_t len, uint16_t *out, uint16_t max_w, uint16_t max_h, uint16_t &w, uint16_t &h) {
  static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
  static uint8_t *pool = (uint8_t *)heap_caps_malloc(POOL_SIZE, MALLOC_CAP_SPIRAM);
  if (!pool) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  Io io = {data, len, 0, out, 0, 0, 0, 0};
  JDEC jd;
  bool ok = jd_prepare(&jd, readData, pool, POOL_SIZE, &io) == JDR_OK && jd.width && jd.height;
  if (ok) {
    // Taille finale : tenir dans la boîte, sans agrandir
    float f = min(1.0f, min((float)max_w / jd.width, (float)max_h / jd.height));
    io.w = w = max(1, (int)lroundf(jd.width * f));
    io.h = h = max(1, (int)lroundf(jd.height * f));
    // Le décodeur réduit lui-même par 2, 4 ou 8 : autant qu'il peut sans passer sous la taille voulue
    uint8_t scale = 0;
    while (scale < 3 && (jd.width >> (scale + 1)) >= w && (jd.height >> (scale + 1)) >= h) scale++;
    io.src_w = (jd.width + (1 << scale) - 1) >> scale;
    io.src_h = (jd.height + (1 << scale) - 1) >> scale;
    memset(out, 0, (size_t)w * h * sizeof(uint16_t));
    ok = jd_decomp(&jd, writeBlock, scale) == JDR_OK;
  }
  xSemaphoreGive(lock);
  return ok;
}
