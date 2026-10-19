// Host addition (not Airwindows): exact replacement for
//   frexpf(x, &e); ldexp(1.0, e + 62)
// used by the 32-bit floating-point dither of the Airwindows ports. For normal
// finite floats the exponent comes straight from the IEEE bits, so the result
// is bit-identical without two libm calls per channel per sample. Zero,
// denormal, infinite and NaN inputs take the original libm route.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
namespace insert_dsp {
static inline double ditherScale(float x) {
  uint32_t bits;
  memcpy(&bits, &x, sizeof bits);
  const uint32_t field = (bits >> 23) & 0xffu;
  if (field == 0 || field == 0xffu) {
    int expon;
    frexpf(x, &expon);
    return ldexp(1.0, expon + 62);
  }
  const uint64_t scaled = (uint64_t)((int)field - 126 + 62 + 1023) << 52;
  double result;
  memcpy(&result, &scaled, sizeof result);
  return result;
}
}  // namespace insert_dsp
