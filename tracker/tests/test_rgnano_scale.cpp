#include "doctest.h"

#include <stdint.h>
#include <stdlib.h>
#include <vector>

#include "../platforms/sdl2/rgnano_scale.h"

namespace {
// Max per-channel difference in RGB565 field units (R,B: 5 bit; G: 6 bit).
int maxFieldDiff(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b, long* exact) {
  int worst = 0;
  for (size_t i = 0; i < a.size(); i++) {
    int dr = abs((a[i] >> 11) - (b[i] >> 11));
    int dg = abs(((a[i] >> 5) & 63) - ((b[i] >> 5) & 63));
    int db = abs((a[i] & 31) - (b[i] & 31));
    worst = std::max(worst, std::max(dr, std::max(dg, db)));
    if (a[i] == b[i]) (*exact)++;
  }
  return worst;
}

void compare(const std::vector<uint32_t>& src, const char* what) {
  rgnanoscale::GenericScaler generic;
  std::vector<uint16_t> a(240 * 240), b(240 * 240);
  generic.scale(src.data(), 320, 240, 240, 240, a.data());
  for (int y = 0; y < 240; y++) rgnanoscale::squashRow320to240(src.data() + y * 320, b.data() + y * 240);
  long exact = 0;
  int worst = maxFieldDiff(a, b, &exact);
  INFO(what << ": exact " << exact << " of " << a.size());
  CHECK(worst <= 1);
}
}  // namespace

TEST_CASE("rgnano 320x240 fast path matches the generic filter") {
  srand(12345);
  std::vector<uint32_t> src(320 * 240);
  for (int frame = 0; frame < 20; frame++) {
    for (auto& p : src) p = ((uint32_t)rand() << 8) ^ (uint32_t)rand();
    compare(src, "random");
  }
  // extremes
  std::fill(src.begin(), src.end(), 0x00FFFFFFu); compare(src, "white");
  std::fill(src.begin(), src.end(), 0x00000000u); compare(src, "black");
  for (int y = 0; y < 240; y++)
    for (int x = 0; x < 320; x++) src[y * 320 + x] = ((x & 1) ? 0x00FFFFFFu : 0) ;
  compare(src, "1px stripes");
  // UI-like: flat panels, 1-pixel lines and 8x12 glyph-ish blocks
  for (int y = 0; y < 240; y++)
    for (int x = 0; x < 320; x++) {
      uint32_t c = 0x00203040u;
      if (((x / 8) + (y / 12)) % 3 == 0 && (x % 8) < 5 && (y % 12) < 9) c = 0x00E0E8F0u;
      if (y % 12 == 11) c = 0x00FF8000u;
      src[y * 320 + x] = c;
    }
  compare(src, "glyph blocks");
  for (int y = 0; y < 240; y++)
    for (int x = 0; x < 320; x++) src[y * 320 + x] = ((x * 255 / 319) << 16) | ((y * 255 / 239) << 8) | ((x + y) & 255);
  compare(src, "gradient");
}

TEST_CASE("rgnano rowsDiffer ignores alpha and finds single changes") {
  std::vector<uint32_t> a(320, 0x11223344u), b(320, 0xFF223344u);
  CHECK(!rgnanoscale::rowsDiffer(a.data(), b.data(), 320));
  b[319] ^= 1;
  CHECK(rgnanoscale::rowsDiffer(a.data(), b.data(), 320));
}
