#include "png.h"
#include <miniz.h>

#define MAX_RAW  (2 * 1024 * 1024)  // image décompressée : 330 x 330 en RGBA 16 bits en fait 870 Ko

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

struct Chunk {
  uint32_t len;
  const char *type;
  const uint8_t *data;
};

// Parcourt les morceaux du fichier : appelle `f` pour chacun, jusqu'à ce qu'elle renvoie faux
template <typename F>
static bool chunks(const uint8_t *data, size_t len, F f) {
  static const uint8_t SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (len < 8 || memcmp(data, SIGNATURE, 8) != 0) return false;
  for (size_t pos = 8; pos + 12 <= len;) {
    Chunk c = {be32(data + pos), (const char *)data + pos + 4, data + pos + 8};
    if (c.len > len - pos - 12) return false;
    if (!f(c)) return true;
    pos += 12 + c.len;
  }
  return true;
}

bool pngHasAlpha(const uint8_t *data, size_t len) {
  bool alpha = false;
  chunks(data, len, [&](const Chunk &c) {
    if (memcmp(c.type, "IHDR", 4) == 0 && c.len >= 13) alpha = c.data[9] == 4 || c.data[9] == 6;
    if (memcmp(c.type, "tRNS", 4) == 0) alpha = true;
    return memcmp(c.type, "IDAT", 4) != 0;
  });
  return alpha;
}

// Défait le filtre d'une ligne, d'après la ligne d'avant (nulle pour la première)
static void unfilter(uint8_t type, uint8_t *row, const uint8_t *prev, size_t n, uint8_t bpp) {
  for (size_t i = 0; i < n; i++) {
    int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
    switch (type) {
      case 1: row[i] += a; break;
      case 2: row[i] += b; break;
      case 3: row[i] += (a + b) / 2; break;
      case 4: {
        int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
        row[i] += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
        break;
      }
    }
  }
}

bool pngDecode(const uint8_t *data, size_t len, uint16_t *out, uint16_t max_w, uint16_t max_h, uint16_t &w, uint16_t &h,
               uint16_t bg) {
  static SemaphoreHandle_t lock = xSemaphoreCreateMutex();
  // Le décompresseur fait 11 Ko : en PSRAM, pas sur la pile
  static tinfl_decompressor *inflator = (tinfl_decompressor *)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
  if (!inflator) return false;

  // En-tête, palette, transparence, et les données compressées, mises bout à bout
  uint32_t width = 0, height = 0;
  uint8_t depth = 0, type = 0, interlace = 0;
  uint8_t palette[256][4];
  memset(palette, 255, sizeof(palette));
  int32_t key[3] = {-1, -1, -1};  // couleur transparente d'une image sans alpha
  uint8_t *zdata = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!zdata) return false;
  size_t zlen = 0;
  bool read = chunks(data, len, [&](const Chunk &c) {
    if (memcmp(c.type, "IHDR", 4) == 0 && c.len >= 13) {
      width = be32(c.data), height = be32(c.data + 4), depth = c.data[8], type = c.data[9], interlace = c.data[12];
    } else if (memcmp(c.type, "PLTE", 4) == 0) {
      for (uint32_t i = 0; i < c.len / 3 && i < 256; i++) memcpy(palette[i], c.data + i * 3, 3);
    } else if (memcmp(c.type, "tRNS", 4) == 0) {
      if (type == 3) {
        for (uint32_t i = 0; i < c.len && i < 256; i++) palette[i][3] = c.data[i];
      } else {
        for (uint32_t i = 0; i < 3 && i * 2 + 1 < c.len; i++) key[i] = c.data[i * 2] << 8 | c.data[i * 2 + 1];
      }
    } else if (memcmp(c.type, "IDAT", 4) == 0) {
      memcpy(zdata + zlen, c.data, c.len);
      zlen += c.len;
    }
    return memcmp(c.type, "IEND", 4) != 0;
  });
  static const uint8_t CHANNELS[7] = {1, 0, 3, 1, 2, 0, 4};
  uint8_t channels = type <= 6 ? CHANNELS[type] : 0;
  size_t stride = ((size_t)width * channels * depth + 7) / 8, raw_len = (stride + 1) * height;
  if (!read || !width || !height || !channels || interlace || raw_len > MAX_RAW || (depth != 8 && depth != 16 && type != 0 && type != 3)) {
    free(zdata);
    return false;
  }
  uint8_t *raw = (uint8_t *)heap_caps_malloc(raw_len, MALLOC_CAP_SPIRAM);
  if (!raw) {
    free(zdata);
    return false;
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  tinfl_init(inflator);
  size_t in = zlen, got = raw_len;
  tinfl_status status = tinfl_decompress(inflator, zdata, &in, raw, raw, &got,
                                         TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  xSemaphoreGive(lock);
  free(zdata);
  bool ok = status == TINFL_STATUS_DONE && got == raw_len;

  // Lignes défiltrées sur place : chacune commence par son octet de filtre
  uint8_t bpp = max(1, channels * depth / 8);
  for (uint32_t y = 0; ok && y < height; y++) {
    uint8_t *row = raw + y * (stride + 1);
    if (row[0] > 4) ok = false;
    else unfilter(row[0], row + 1, y ? row - stride : nullptr, stride, bpp);
  }

  if (ok) {
    float f = min(1.0f, min((float)max_w / width, (float)max_h / height));
    w = max(1, (int)lroundf(width * f));
    h = max(1, (int)lroundf(height * f));
    uint32_t bg_r = bg >> 11 << 3, bg_g = (bg >> 5 & 0x3F) << 2, bg_b = (bg & 0x1F) << 3;
    // Valeur du canal k du pixel x d'une ligne, ramenée sur 8 bits
    auto sample = [&](const uint8_t *row, uint32_t x, uint8_t k) -> uint32_t {
      if (depth == 16) return row[(x * channels + k) * 2];
      if (depth == 8) return row[x * channels + k];
      uint32_t bit = x * depth, v = row[bit / 8] >> (8 - depth - bit % 8) & ((1 << depth) - 1);
      return type == 3 ? v : v * 255 / ((1 << depth) - 1);
    };
    auto raw16 = [&](const uint8_t *row, uint32_t x, uint8_t k) -> int32_t {
      if (depth == 16) return row[(x * channels + k) * 2] << 8 | row[(x * channels + k) * 2 + 1];
      if (depth == 8) return row[x * channels + k];
      uint32_t bit = x * depth;
      return row[bit / 8] >> (8 - depth - bit % 8) & ((1 << depth) - 1);
    };
    // Chaque pixel voulu est la moyenne des pixels de l'image qu'il couvre, posés sur le fond
    for (uint32_t ty = 0; ty < h; ty++) {
      uint32_t y0 = ty * height / h, y1 = max(y0 + 1, (ty + 1) * height / h);
      for (uint32_t tx = 0; tx < w; tx++) {
        uint32_t x0 = tx * width / w, x1 = max(x0 + 1, (tx + 1) * width / w);
        uint32_t r = 0, g = 0, b = 0, n = 0;
        for (uint32_t y = y0; y < y1; y++) {
          const uint8_t *row = raw + y * (stride + 1) + 1;
          for (uint32_t x = x0; x < x1; x++, n++) {
            uint32_t pr, pg, pb, a = 255;
            if (type == 3) {
              const uint8_t *p = palette[sample(row, x, 0)];
              pr = p[0], pg = p[1], pb = p[2], a = p[3];
            } else if (type == 0 || type == 4) {
              pr = pg = pb = sample(row, x, 0);
              if (type == 4) a = sample(row, x, 1);
              else if (raw16(row, x, 0) == key[0]) a = 0;
            } else {
              pr = sample(row, x, 0), pg = sample(row, x, 1), pb = sample(row, x, 2);
              if (type == 6) a = sample(row, x, 3);
              else if (raw16(row, x, 0) == key[0] && raw16(row, x, 1) == key[1] && raw16(row, x, 2) == key[2]) a = 0;
            }
            r += (pr * a + bg_r * (255 - a)) / 255;
            g += (pg * a + bg_g * (255 - a)) / 255;
            b += (pb * a + bg_b * (255 - a)) / 255;
          }
        }
        r /= n, g /= n, b /= n;
        out[ty * w + tx] = (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3;
      }
    }
  }
  free(raw);
  return ok;
}
