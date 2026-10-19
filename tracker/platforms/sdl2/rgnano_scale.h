// RG Nano presenter filters: pure functions, no SDL/fb dependencies, so the
// native tests can compare the fast path against the generic filter.
#ifndef RGNANO_SCALE_H
#define RGNANO_SCALE_H

#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <vector>

namespace rgnanoscale {

// Exact fractional box filter tables for one axis.
struct Axis {
  int source = 0;
  std::vector<int> first;     // first source index per destination index
  std::vector<int> count;
  std::vector<int> offset;    // into weights
  std::vector<uint16_t> weights;
  void build(int src, int dst) {
    source = src;
    first.assign(dst, 0);
    count.assign(dst, 0);
    offset.assign(dst, 0);
    weights.clear();
    for (int i = 0; i < dst; i++) {
      long long lo = (long long)i * src, hi = (long long)(i + 1) * src;
      int j0 = (int)(lo / dst), j1 = (int)((hi - 1) / dst);
      first[i] = j0;
      count[i] = j1 - j0 + 1;
      offset[i] = (int)weights.size();
      for (int j = j0; j <= j1; j++) {
        long long a = (long long)j * dst, b = a + dst;
        weights.push_back((uint16_t)((hi < b ? hi : b) - (lo > a ? lo : a)));
      }
    }
  }
};

// Generic separable box (area) filter, w x h source (tightly packed, low 24 bits
// are RGB) -> dw x dh RGB565. Every destination pixel covers an exact fractional
// source area: weights per axis sum to the source length.
struct GenericScaler {
  Axis axisX, axisY;
  int tableW = 0, tableH = 0, dstW = 0, dstH = 0;
  std::vector<uint32_t> hrow, acc;

  void scale(const uint32_t* argb, int w, int h, int dw, int dh, uint16_t* out565) {
    if (w != tableW || h != tableH || dw != dstW || dh != dstH) {
      axisX.build(w, dw);
      axisY.build(h, dh);
      tableW = w; tableH = h; dstW = dw; dstH = dh;
    }
    hrow.resize((size_t)dw * 3);
    acc.resize((size_t)dw * 3);
    const uint64_t divisor = (uint64_t)w * (uint64_t)h;
    const uint64_t reciprocal = (1ULL << 32) / divisor;
    int cachedRow = -1;
    for (int dy = 0; dy < dh; dy++) {
      std::fill(acc.begin(), acc.end(), 0u);
      for (int k = 0; k < axisY.count[dy]; k++) {
        const int sy = axisY.first[dy] + k;
        const uint32_t wy = axisY.weights[axisY.offset[dy] + k];
        if (sy != cachedRow) {
          const uint32_t* line = argb + (size_t)sy * w;
          for (int dx = 0; dx < dw; dx++) {
            uint32_t r = 0, g = 0, b = 0;
            const uint16_t* wx = &axisX.weights[axisX.offset[dx]];
            const uint32_t* px = line + axisX.first[dx];
            for (int j = axisX.count[dx]; j > 0; j--, px++, wx++) {
              const uint32_t p = *px, wt = *wx;
              r += ((p >> 16) & 0xff) * wt;
              g += ((p >> 8) & 0xff) * wt;
              b += (p & 0xff) * wt;
            }
            hrow[dx * 3] = r; hrow[dx * 3 + 1] = g; hrow[dx * 3 + 2] = b;
          }
          cachedRow = sy;
        }
        for (int i = 0; i < dw * 3; i++) acc[i] += hrow[i] * wy;
      }
      uint16_t* out = out565 + (size_t)dy * dw;
      for (int dx = 0; dx < dw; dx++) {
        uint32_t r = (uint32_t)(((uint64_t)acc[dx * 3] * reciprocal + (1ULL << 31)) >> 32);
        uint32_t g = (uint32_t)(((uint64_t)acc[dx * 3 + 1] * reciprocal + (1ULL << 31)) >> 32);
        uint32_t b = (uint32_t)(((uint64_t)acc[dx * 3 + 2] * reciprocal + (1ULL << 31)) >> 32);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        out[dx] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
      }
    }
  }
};

// Fast path for the fixed 320 -> 240 case: vertical 1:1, horizontally 4 source
// pixels -> 3 destination pixels with weights (3,1)/4, (1,1)/2, (1,3)/4, rounded
// to nearest, straight into RGB565. Two channels share each 32-bit lane
// (R and B in 0x00FF00FF) so the whole average is a handful of integer ops.
static inline uint16_t pack565(uint32_t rb, uint32_t g) {
  // rb: B at bits 0-7, R at bits 16-23 (xRGB, after averaging); g: 0-255.
  return (uint16_t)(((rb >> 8) & 0xF800u) | ((g << 3) & 0x07E0u) | ((rb >> 3) & 0x1Fu));
}

static inline void squashRow320to240(const uint32_t* src, uint16_t* dst) {
  const uint32_t M = 0x00FF00FFu;
  for (int i = 0; i < 80; i++, src += 4, dst += 3) {
    const uint32_t p0 = src[0], p1 = src[1], p2 = src[2], p3 = src[3];
    const uint32_t rb0 = p0 & M, rb1 = p1 & M, rb2 = p2 & M, rb3 = p3 & M;
    const uint32_t g0 = (p0 >> 8) & 0xFF, g1 = (p1 >> 8) & 0xFF, g2 = (p2 >> 8) & 0xFF, g3 = (p3 >> 8) & 0xFF;
    // Lane sums stay below 2^16 (max 1020 + rounding), so lanes never carry.
    const uint32_t a0 = (((rb0 * 3 + rb1 + 0x00020002u) >> 2) & M);
    const uint32_t a1 = (((rb1 + rb2 + 0x00010001u) >> 1) & M);
    const uint32_t a2 = (((rb2 + rb3 * 3 + 0x00020002u) >> 2) & M);
    dst[0] = pack565(a0, (g0 * 3 + g1 + 2) >> 2);
    dst[1] = pack565(a1, (g1 + g2 + 1) >> 1);
    dst[2] = pack565(a2, (g2 + g3 * 3 + 2) >> 2);
  }
}

// True when any word of the two rows differs. No early out: branch-free OR
// accumulation is faster than a data-dependent exit on an in-order core and the
// rows are only 1280 bytes.
static inline bool rowsDiffer(const uint32_t* a, const uint32_t* b, int words) {
  uint32_t acc = 0;
  for (int i = 0; i < words; i++) acc |= (a[i] ^ b[i]) & 0x00FFFFFFu;
  return acc != 0;
}

}  // namespace rgnanoscale

#endif
