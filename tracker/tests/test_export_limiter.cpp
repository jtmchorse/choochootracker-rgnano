#include "doctest.h"
#include "chipnomad_lib.h"
#include "pitch_table_utils.h"
#include "export/export.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Loud one-phrase AY song; mixVolume pushes the bus well past full scale.
struct ExportFixture {
  Project p;
  ExportFixture() {
    ChipNomadState* s = chipnomadCreate();
    p = s->project;
    chipnomadDestroy(s);
    p.tickRate = 50;
    p.chipType = ChipType::AY;
    p.chipsCount = 1;
    p.chipSetup.ay = (ChipSetupAY){ .clock = 1773400, .isYM = 0, .stereoMode = StereoModeAY::ABC, .stereoSeparation = 50, .pwmFullRange = 0 };
    p.tracksCount = projectGetTotalTracks(&p);
    p.linearPitch = 0;
    calculatePitchTableAY(&p);
    Instrument& in = p.instruments[0];
    in.type = InstrumentType::AY1;
    in.tableSpeed = 1;
    in.transposeEnabled = 1;
    in.chip.ay.volumeEnvelope.type = ModulationType::ADSR;
    in.chip.ay.volumeEnvelope.amount = 127;
    in.chip.ay.volumeEnvelope.p1 = 15; in.chip.ay.volumeEnvelope.p2 = 0;
    in.chip.ay.volumeEnvelope.p3 = 15; in.chip.ay.volumeEnvelope.p4 = 0;
    in.chip.ay.defaultMixer = 0x01;
    for (int r = 0; r < 16; ++r) {
      p.phrases[0].rows[r].note = 48;
      p.phrases[0].rows[r].instrument = 0;
      p.phrases[0].rows[r].volume = PHRASE_VOLUME_MAX;
    }
    p.chains[0].rows[0].phrase = 0;
    p.song[0][0] = 0;
  }

  // Export to a temp WAV, return interleaved S16 samples.
  std::vector<int16_t> run(float mixVolume, int limiter, size_t* dataBytes = nullptr) {
    const char* path = "/tmp/ccr_export_limiter_test.wav";
    ExporterWAV ex(path, &p, 0, 32000, 16, mixVolume, false, limiter);
    while (ex.next() >= 0) {}
    ex.finish();
    std::vector<int16_t> out;
    FILE* f = fopen(path, "rb");
    REQUIRE(f);
    fseek(f, 44, SEEK_SET);
    int16_t v;
    while (fread(&v, 2, 1, f) == 1) out.push_back(v);
    fclose(f);
    remove(path);
    if (dataBytes) *dataBytes = out.size() * 2;
    return out;
  }
};

}  // namespace

TEST_CASE("export: limiter on keeps an over-full-scale mix below the ceiling") {
  ExportFixture fx;
  std::vector<int16_t> off = fx.run(40.0f, 0);
  size_t clippedOff = 0;
  for (int16_t s : off) if (s >= 32767 || s <= -32768) ++clippedOff;
  REQUIRE(clippedOff > 0);  // the fixture really does clip without the limiter

  std::vector<int16_t> on = fx.run(40.0f, 1);
  size_t clipped = 0; int peak = 0;
  for (int16_t s : on) {
    if (s >= 32767 || s <= -32768) ++clipped;
    peak = std::max(peak, std::abs((int)s));
  }
  CHECK(clipped == 0);
  CHECK(peak <= (int)std::ceil(32768.0 * std::pow(10.0, -0.3 / 20.0)));
  CHECK(peak > 1000);
  CHECK(on.size() == off.size());  // lookahead compensated: same length
}

TEST_CASE("export: limiter off is bit-identical to the unlimited render") {
  ExportFixture fx;
  std::vector<int16_t> a = fx.run(0.5f, 0);
  std::vector<int16_t> b = fx.run(0.5f, 0);
  CHECK(a == b);
  // Below the ceiling the limiter only delays-and-restores: same samples.
  std::vector<int16_t> c = fx.run(0.5f, 1);
  CHECK(c == a);
}
