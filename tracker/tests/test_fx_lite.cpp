#include <string>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
#include "beads_pool.h"
#include "common.h"
#include "doctest.h"
#include "fx_profile.h"
#include "insert_fx.h"
#include "external/insert_fx/dither_scale.h"

namespace {
struct LiteScope {
  bool previous = fxLiteEnabled();
  explicit LiteScope(bool on) { fxLiteSet(on); }
  ~LiteScope() { fxLiteSet(previous); }
};

struct InsertRig {
  BeadsPool pool;  // declared first: the chain's lease must go before the pool
  InsertChain chain{32000.0f};
  InsertConfig config[2] = {};
  InsertAutomation automation = {};
  uint8_t effective[2][8] = {};
  explicit InsertRig(int module) {
    insertSelect(&config[0], module);
    insertSelect(&config[1], insertOff);
    memcpy(effective[0], config[0].values, 8);
    if (module == insertBeads) chain.attachBeads(0, pool.prepare(0, 32000.0f));
    chain.sync(config, &automation, true);
  }
  // Renders `frames` of a saw (or silence) in 256-frame callbacks and returns the last output peak.
  float run(int frames, bool signalOn, uint64_t* skipped = nullptr) {
    std::vector<float> data(512);
    float phase = 0, peak = 0;
    FxProfile before;
    chipnomadGetFxProfile(&before, true);
    for (int done = 0; done < frames; done += 256) {
      for (int i = 0; i < 256; ++i) {
        phase += 110.0f / 32000.0f;
        if (phase >= 1) phase -= 1;
        data[2 * i] = data[2 * i + 1] = signalOn ? (phase * 2 - 1) * 0.3f : 0.0f;
      }
      chain.process(data.data(), 256, effective);
      peak = 0;
      for (float x : data) peak = std::max(peak, fabsf(x));
    }
    FxProfile after;
    chipnomadGetFxProfile(&after, true);
    if (skipped) *skipped = after.skippedFrames;
    return peak;
  }
};
}  // namespace

TEST_SUITE("fx lite") {
  TEST_CASE("dither scale equals frexpf/ldexp for every kind of float") {
    const float samples[] = {0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 3.14159f, 1e-20f, 1e-38f, 1e-42f, 1e30f,
                             3.4e38f, 0.123456f, -7.7e-9f, INFINITY, -INFINITY};
    for (float x : samples) {
      int e;
      frexpf(x, &e);
      CHECK(insert_dsp::ditherScale(x) == ldexp(1.0, e + 62));
    }
    uint32_t state = 1;
    for (int i = 0; i < 200000; ++i) {
      state = state * 1664525u + 1013904223u;
      float x;
      memcpy(&x, &state, 4);
      if (std::isnan(x)) continue;
      int e;
      frexpf(x, &e);
      REQUIRE(insert_dsp::ditherScale(x) == ldexp(1.0, e + 62));
    }
  }

  TEST_CASE("lite off never bypasses an insert") {
    LiteScope off(false);
    InsertRig a(insertOTT);
    uint64_t skipped = 1;
    a.run(32000, true);
    float quiet = a.run(64000, false, &skipped);
    CHECK(skipped == 0);
    CHECK(quiet < 1e-3f);
  }

  TEST_CASE("lite bypasses a silent insert after it has gone quiet and wakes cleanly") {
    LiteScope on(true);
    InsertRig c(insertChorus);
    uint64_t skipped = 0;
    c.run(32000, true);
    c.run(3 * 32000, false, &skipped);
    CHECK(skipped > 2 * 32000);  // half a second of hold, then bypassed
    float woke = c.run(8000, true, &skipped);
    CHECK(skipped == 0);
    CHECK(woke > 0.05f);
    CHECK(woke < 1.0f);
  }

  TEST_CASE("lite Beads waits for its buffer to empty before bypassing") {
    LiteScope on(true);
    InsertRig c(insertBeads);
    uint64_t skipped = 0;
    c.run(32000, true);
    c.run(3 * 32000, false, &skipped);
    CHECK(skipped == 0);  // four seconds of buffer could still replay
    c.run(3 * 32000, false, &skipped);
    CHECK(skipped > 32000);
  }

  TEST_CASE("lite Beads produces audio comparable to the full engine") {
    float peaks[2];
    for (int lite = 0; lite < 2; ++lite) {
      LiteScope scope(lite != 0);
      InsertRig c(insertBeads);
      c.config[0].values[7] = 255;  // full wet
      memcpy(c.effective[0], c.config[0].values, 8);
      c.run(2 * 32000, true);
      peaks[lite] = c.run(32000, true);
      CHECK(peaks[lite] > 0.05f);
      CHECK(peaks[lite] < 2.0f);
    }
  }

  TEST_CASE("lite OTT stays within a fraction of a dB of the full path") {
    float rms[2];
    for (int lite = 0; lite < 2; ++lite) {
      LiteScope scope(lite != 0);
      InsertRig c(insertOTT);
      std::vector<float> data(512);
      float phase = 0;
      double sum = 0;
      long count = 0;
      for (int block = 0; block < 400; ++block) {
        for (int i = 0; i < 256; ++i) {
          phase += 220.0f / 32000.0f;
          if (phase >= 1) phase -= 1;
          float env = block % 100 < 50 ? 0.4f : 0.03f;
          data[2 * i] = data[2 * i + 1] = sinf(phase * 6.2831853f) * env;
        }
        c.chain.process(data.data(), 256, c.effective);
        if (block >= 100)
          for (float x : data) { sum += x * x; ++count; }
      }
      rms[lite] = (float)sqrt(sum / count);
    }
    CHECK(fabs(20 * log10(rms[1] / rms[0])) < 0.5);
  }

#ifdef RGNANO_BUILD
  TEST_CASE("overload governor bypasses the costliest live slot and spares offline chains") {
    fxGovernorSetLimit(0.000001);
    for (int live = 0; live < 2; ++live) {
      InsertRig c(insertBeads);
      c.chain.setRealtime(live != 0);
      FxProfile before;
      chipnomadGetFxProfile(&before, false);
      // Render for over half a second of wall time so one governor window closes.
      auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(700)) c.run(256, true);
      FxProfile after;
      chipnomadGetFxProfile(&after, false);
      if (live) {
        CHECK(after.shedEvents > before.shedEvents);
        CHECK(after.shedModule == (uint32_t)insertBeads);
        // The slot has faded out: the chain now passes the dry signal untouched.
        float dry = c.run(2048, true);
        CHECK(dry <= 0.31f);
        // Touching the slot gives it another chance.
        c.config[0].values[2] ^= 1;
        ++c.config[0].edits[2];
        c.chain.sync(c.config, &c.automation, true);
      } else {
        CHECK(after.shedEvents == before.shedEvents);
      }
    }
    fxGovernorSetLimit(0.70);
  }
#endif

  TEST_CASE("play FPS and FX lite survive a settings save and load") {
    const int savedFps = appSettings.playFps, savedLite = appSettings.fxLite;
    const bool savedFlag = fxLiteEnabled();
    appSettings.playFps = 15;
    appSettings.fxLite = 1;
    REQUIRE(settingsSave() == 0);
    appSettings.playFps = 20;
    appSettings.fxLite = 0;
    fxLiteSet(false);
    REQUIRE(settingsLoad() == 0);
    CHECK(appSettings.playFps == 15);
    CHECK(appSettings.fxLite == 1);
    CHECK(fxLiteEnabled());
    appSettings.playFps = savedFps;
    appSettings.fxLite = savedLite;
    REQUIRE(settingsSave() == 0);
    fxLiteSet(savedFlag);
  }
}

// Every insert id has a named profile slot, so the astat fx line and the
// overload governor can see it (formant, id 16, used to fall off the end).
#include "insert_fx.h"
TEST_CASE("fx profile table covers every insert id") {
  CHECK((int)FxProfile::insertSlots == (int)insertModuleCount);
  for (int m = 1; m < insertModuleCount; ++m) {
    const char* name = fxProfileModuleName(m);
    REQUIRE(name != nullptr);
    CHECK(std::string(name) != "?");
    CHECK(std::string(name) != "");
    for (int o = 1; o < m; ++o) CHECK(std::string(name) != std::string(fxProfileModuleName(o)));
    FxProfile p = {};
    p.insertNs[m] = 5000000;  // 5 ms over 1 callback
    char line[256];
    fxProfileFormat(p, 1, line, sizeof(line));
    CHECK(std::string(line).find(std::string(name) + "=5000") != std::string::npos);
  }
  CHECK(std::string(fxProfileModuleName(insertModuleCount)) == "?");
  FxProfile shed = {};
  shed.shedEvents = 1;
  shed.shedModule = insertFormant;
  char line[256];
  fxProfileFormat(shed, 1, line, sizeof(line));
  CHECK(std::string(line).find("last form") != std::string::npos);
}
