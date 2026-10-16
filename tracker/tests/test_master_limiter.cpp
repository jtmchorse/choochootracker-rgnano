#include "doctest.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "chipnomad_lib.h"
#include "synth/master_limiter.h"

namespace {
const float kPi = 3.14159265358979f;

// Interleaved stereo test signal: a sine each side at its own frequency.
std::vector<float> sine(int frames, float ampL, float ampR, float rate = 32000.0f) {
  std::vector<float> v((size_t)frames * 2);
  for (int i = 0; i < frames; ++i) {
    v[2 * i] = ampL * sinf(2 * kPi * 110.0f * i / rate);
    v[2 * i + 1] = ampR * sinf(2 * kPi * 165.0f * i / rate + 0.7f);
  }
  return v;
}
}  // namespace

TEST_CASE("master limiter passes non-clipping material unchanged (gain 1.0, delayed)") {
  MasterLimiter lim;
  lim.init(32000.0f);
  const int n = 32000;
  std::vector<float> in = sine(n, 0.8f, 0.6f), out = in;
  lim.process(out.data(), n);
  const int d = lim.latency();
  REQUIRE(d > 0);
  // Bit-exact after the fixed lookahead delay; the lead-in is silence.
  for (int i = 0; i < d * 2; ++i) CHECK(out[i] == 0.0f);
  size_t mismatches = 0;
  for (int i = 0; i < (n - d) * 2; ++i) if (out[(size_t)d * 2 + i] != in[i]) ++mismatches;
  CHECK(mismatches == 0);
  MasterLimiter::Stats s = lim.takeStats();
  CHECK(s.minGain == 1.0f);
  CHECK(s.meanReduction == 0.0f);
  CHECK(s.engagedFrames == 0);
  CHECK(s.totalFrames == (uint32_t)n);
}

TEST_CASE("master limiter holds the ceiling and never emits NaN") {
  MasterLimiter lim;
  lim.init(32000.0f);
  const float ceiling = lim.ceiling();
  CHECK(ceiling == doctest::Approx(0.96605f).epsilon(1e-4));  // -0.3 dBFS
  std::vector<float> v = sine(32000, 3.0f, 2.5f);
  v[400] = 1.0e9f; v[801] = -50.0f; v[1000] = NAN;
  lim.process(v.data(), 32000);
  float peak = 0;
  for (float x : v) { CHECK(x == x); peak = fmaxf(peak, fabsf(x)); }
  CHECK(peak <= ceiling);
  MasterLimiter::Stats s = lim.takeStats();
  CHECK(s.minGain < 0.5f);
  CHECK(s.meanReduction > 0.0f);
  CHECK(s.engagedFrames > 0);
}

TEST_CASE("master limiter is stereo-linked and chunk-size independent") {
  const int n = 8000;
  std::vector<float> in = sine(n, 2.0f, 0.4f);
  MasterLimiter a, b;
  a.init(32000.0f); b.init(32000.0f);
  std::vector<float> whole = in, chunked = in;
  a.process(whole.data(), n);
  for (int pos = 0, step = 1; pos < n; pos += step, step = step % 97 + 1) {
    int c = pos + step > n ? n - pos : step;
    b.process(chunked.data() + (size_t)pos * 2, c);
  }
  CHECK(std::memcmp(whole.data(), chunked.data(), whole.size() * sizeof(float)) == 0);
  // Same gain on both sides: output ratio equals input ratio (delayed).
  const int d = a.latency();
  for (int i = d; i < n; ++i) {
    float inL = in[(size_t)(i - d) * 2], inR = in[(size_t)(i - d) * 2 + 1];
    if (fabsf(inL) < 0.05f || fabsf(inR) < 0.05f) continue;
    float gl = whole[(size_t)i * 2] / inL, gr = whole[(size_t)i * 2 + 1] / inR;
    REQUIRE(gl == doctest::Approx(gr).epsilon(1e-4));
  }
}

TEST_CASE("master limiter releases smoothly and returns to exact unity") {
  MasterLimiter lim;
  lim.init(32000.0f);
  // 50 ms hot burst, then quiet material for 1.5 s.
  std::vector<float> v = sine(1600, 2.0f, 2.0f);
  std::vector<float> quiet = sine(48000, 0.3f, 0.3f);
  v.insert(v.end(), quiet.begin(), quiet.end());
  const std::vector<float> in = v;
  lim.process(v.data(), (int)(v.size() / 2));
  const int d = lim.latency();
  // Gain applied to quiet samples (input amplitude 0.3): early deep, then recovered.
  auto gainAt = [&](int frame) {
    size_t i = (size_t)frame * 2;
    return fabsf(in[i - (size_t)d * 2]) > 0.05f ? v[i] / in[i - (size_t)d * 2] : -1.0f;
  };
  float prev = 0.0f; int checked = 0;
  for (int f = 1700; f < 1600 + 48000; f += 7) {
    float g = gainAt(f);
    if (g < 0) continue;
    CHECK(g >= prev - 1e-3f);  // monotonic recovery, no pumping
    prev = g; ++checked;
  }
  CHECK(checked > 100);
  // After ~1.5 s the gain is exactly 1: output == delayed input.
  size_t tail = (size_t)(1600 + 48000 - 400) * 2;
  CHECK(v[tail] == in[tail - (size_t)d * 2]);
  CHECK(v[tail + 1] == in[tail + 1 - (size_t)d * 2]);
}

TEST_CASE("master limiter: platform default, fresh engine state is off, toggle works") {
#ifdef RGNANO_BUILD
  CHECK(chipnomadDefaultMasterLimiter() == 1);
#else
  CHECK(chipnomadDefaultMasterLimiter() == 0);
#endif
  ChipNomadState* s = chipnomadCreate();
  REQUIRE(s != nullptr);
  CHECK(s->masterLimiterEnabled == 0);  // fresh state: offline export and other tests stay bit-exact
  chipnomadSetMasterLimiter(s, 1);
  CHECK(s->masterLimiterEnabled == 1);
  chipnomadSetMasterLimiter(s, 0);
  CHECK(s->masterLimiterEnabled == 0);
  float minGain, meanRed; uint32_t e, t;
  CHECK(chipnomadTakeLimiterStats(s, &minGain, &meanRed, &e, &t) == 1);
  chipnomadDestroy(s);
}
