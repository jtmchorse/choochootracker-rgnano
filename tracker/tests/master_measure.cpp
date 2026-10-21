// Offline master-bus measurement and bounce, for the RG Nano clipping work.
//
//   master_measure <project.cct> <rate> <seconds> <limiter 0|1> [out.wav] [mixVolume%]
//
// Renders through chipnomadRender() exactly as the audio callback does (same
// master path, same float -> S16 conversion as audio_manager.cpp) and prints
// one machine-readable line: clipped S16 samples, peak dBFS, how many tracks
// and which engines played, limiter gain reduction. Not part of run_tests.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "chipnomad_lib.h"
#include "audio_monitor.h"
#include "playback.h"
#include "project.h"
#include "project_utils.h"
#include "screens/screens.h"
#include "synth/master_limiter.h"

void getSelectionBounds(ScreenData*, int*, int*, int*, int*) {}

static void writeWav(const char* path, const std::vector<int16_t>& pcm, int rate) {
  FILE* f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  uint32_t dataBytes = (uint32_t)(pcm.size() * 2), v32;
  uint16_t v16;
  fwrite("RIFF", 1, 4, f); v32 = 36 + dataBytes; fwrite(&v32, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f); v32 = 16; fwrite(&v32, 4, 1, f);
  v16 = 1; fwrite(&v16, 2, 1, f); v16 = 2; fwrite(&v16, 2, 1, f);
  v32 = rate; fwrite(&v32, 4, 1, f); v32 = rate * 4; fwrite(&v32, 4, 1, f);
  v16 = 4; fwrite(&v16, 2, 1, f); v16 = 16; fwrite(&v16, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
  fwrite(pcm.data(), 2, pcm.size(), f);
  fclose(f);
}

// Stand-in for the tester's project (run from tracker/packaging/common so the relative sample paths resolve, like the app does): a breakbeat sample spread over 4 tracks
// (bundled ChocolateAmen slices, no time-stretch, full-scale normalised
// content) plus 4 synth tracks (Braids, Plaits, aChChid, Plaits). Written with
// the project writer so it can also be loaded on a device.
//   master_measure --make-standin out.cct
static void setupTrack(Project* p, int track, InstrumentType type, const char* name) {
  Instrument* ins = &p->instruments[track];
  getInstrumentFunctions(ins->type).free(ins);
  ins->type = type;
  getInstrumentFunctions(type).init(ins);
  snprintf(ins->name, sizeof(ins->name), "%s", name);
  p->song[0][track] = (uint16_t)track;
  for (int r = 0; r < 8; ++r) { p->chains[track].rows[r].phrase = (uint16_t)track; p->chains[track].rows[r].transpose = 0; }
  phraseClear(&p->phrases[track]);
}
static void setNote(Project* p, int track, int row, int note) {
  p->phrases[track].rows[row].note = (uint8_t)note;
  p->phrases[track].rows[row].instrument = (uint8_t)track;
  p->phrases[track].rows[row].volume = 0x7F;
}
static int makeStandin(const char* out) {
  Project* p = (Project*)malloc(sizeof(Project));
  projectInitAY(p);
  p->chipsCount = 8;  // one track per chip slot, like the shipped demos
  p->tracksCount = 8;
  const char* slices[4] = {"samples/ChocolateAmen/01-kik.wav", "samples/ChocolateAmen/03-sn1.wav",
                           "samples/ChocolateAmen/05-sn2.wav", "samples/ChocolateAmen/06-csh.wav"};
  for (int t = 0; t < 4; ++t) {
    setupTrack(p, t, InstrumentType::Sample, "break");
    InstrumentSample* sm = &p->instruments[t].chip.sample;
    snprintf(sm->path, sizeof(sm->path), "%s", slices[t]);
    p->instruments[t].volume = 255;
  }
  // Two-step-ish amen pattern across the four tracks (16 rows).
  const int kick[] = {0, 3, 8, 10}, sn1[] = {4, 12}, sn2[] = {7, 9, 15}, csh[] = {2, 6, 11, 14};
  for (int r : kick) setNote(p, 0, r, 48);
  for (int r : sn1) setNote(p, 1, r, 48);
  for (int r : sn2) setNote(p, 2, r, 48);
  for (int r : csh) setNote(p, 3, r, 48);
  setupTrack(p, 4, InstrumentType::Braids, "bass");
  setupTrack(p, 5, InstrumentType::Plaits, "lead");
  setupTrack(p, 6, InstrumentType::AChChid, "stab");
  setupTrack(p, 7, InstrumentType::Plaits, "pad");
  const int bass[] = {0, 4, 8, 12}, bn[] = {36, 36, 39, 34};
  for (int i = 0; i < 4; ++i) setNote(p, 4, bass[i], bn[i]);
  const int lead[] = {0, 2, 4, 6, 8, 10, 12, 14}, ln[] = {60, 63, 67, 63, 60, 65, 67, 70};
  for (int i = 0; i < 8; ++i) setNote(p, 5, lead[i], ln[i]);
  const int stab[] = {0, 6, 12}, sn[] = {55, 58, 53};
  for (int i = 0; i < 3; ++i) setNote(p, 6, stab[i], sn[i]);
  setNote(p, 7, 0, 48); setNote(p, 7, 8, 51);
  for (int t = 0; t < 8; ++t) p->trackVolume[t] = 100;
  int rc = projectSave(p, out);
  if (rc) fprintf(stderr, "save failed: %s\n", projectFileError);
  free(p);
  return rc;
}

// LFO ramp probe: one sustained note, volume LFO (slot 1) on the chosen engine.
//   master_measure --make-lfo out.cct <period> <shape> <trigger> <braids|ay>
static int makeLfo(const char* out, int period, int shape, int trigger, const char* eng, ModulationType mt = ModulationType::LFO, int mult = 0) {
  Project* p = (Project*)malloc(sizeof(Project));
  projectInitAY(p);
  p->chipsCount = 8;
  p->tracksCount = 8;
  bool ay = !strcmp(eng, "ay");
  setupTrack(p, 0, ay ? InstrumentType::AY1 : InstrumentType::Braids, "lfo");
  setNote(p, 0, 0, 48);
  p->phrases[0].rows[0].volume = (mt == ModulationType::SLFO) ? 0x7F : 0;
  p->chains[0].rows[0].phrase = 0;
  for (int r = 1; r < 8; ++r) p->chains[0].rows[r].phrase = 1;
  phraseClear(&p->phrases[1]);
  Modulation* m = &p->instruments[0].modulation[1];
  m->type = mt;
  m->destination = 1;
  m->amount = 127;
  m->p1 = (uint8_t)shape; m->p2 = (uint8_t)trigger; m->p3 = (uint8_t)period; m->p4 = (uint8_t)mult; m->p5 = 0;
  p->trackVolume[0] = 100;
  int rc = projectSave(p, out);
  if (rc) fprintf(stderr, "save failed: %s\n", projectFileError);
  free(p);
  return rc;
}

int main(int argc, char** argv) {
  if (argc >= 7 && !strcmp(argv[1], "--make-lfo")) {
    ModulationType mt = argc > 7 && !strcmp(argv[7], "sync") ? ModulationType::SLFO : argc > 7 && !strcmp(argv[7], "slow") ? ModulationType::SlowLFO : ModulationType::LFO;
    return makeLfo(argv[2], atoi(argv[3]), atoi(argv[4]), atoi(argv[5]), argv[6], mt, argc > 8 ? atoi(argv[8]) : 0);
  }
  if (argc == 3 && !strcmp(argv[1], "--make-standin")) return makeStandin(argv[2]);
  if (argc < 5) { fprintf(stderr, "usage: %s project rate seconds limiter [out.wav] [mixVolume%%]\n", argv[0]); return 2; }
  const char* path = argv[1];
  const int rate = atoi(argv[2]), seconds = atoi(argv[3]), limiter = atoi(argv[4]);
  const char* wavPath = argc > 5 && argv[5][0] ? argv[5] : nullptr;
  const float mixVolume = argc > 6 ? atof(argv[6]) / 100.0f : 1.0f;

  Project project;
  projectInit(&project);
  if (projectLoad(&project, path)) { fprintf(stderr, "cannot load %s: %s\n", path, projectFileError); return 1; }

  ChipNomadState* state = chipnomadCreate();
  state->ownsProjectResources = 0;
  state->project = project;
  state->audioProject = project;
  playbackInit(&state->playbackState, &state->audioProject);
  chipnomadInitChips(state, rate, nullptr);
  chipnomadSetQuality(state, ChipNomadQuality::medium);  // app default
  chipnomadSetBraidsSettings(state, 6, 0, 0, 12345);     // app defaults (common.cpp)
  state->aySampleDithering = 1;
  state->mixVolume = mixVolume;
  chipnomadSetMasterLimiter(state, limiter);
  const int kBlock = 1024;
  chipnomadReserveRenderBuffers(state, kBlock);
  playbackStartSong(&state->playbackState, 0, 0, 1);

  std::vector<float> buf(kBlock * 2);
  std::vector<int16_t> pcm;
  std::set<std::string> engines;
  std::set<int> tracksSeen;
  long long total = 0, remaining = (long long)seconds * rate, clipped = 0, over1 = 0;
  uint32_t peak = 0;
  float peakF = 0.0f;
  std::vector<long long> winClip;
  long long winFrames = 0, winClipNow = 0;
  while (remaining > 0) {
    int frames = (int)std::min<long long>(remaining, kBlock);
    int got = chipnomadRender(state, buf.data(), frames);
    if (got <= 0) break;
    for (int t = 0; t < state->audioProject.tracksCount; ++t) {
      int ins = state->playbackState.tracks[t].note.instrument;
      if (ins != EMPTY_VALUE_8 && ins < PROJECT_MAX_INSTRUMENTS && state->playbackState.tracks[t].note.volume != 0) {
        tracksSeen.insert(t);
        engines.insert(instrumentTypeName(state->audioProject.instruments[ins].type));
      }
    }
    for (int i = 0; i < got * 2; ++i) {
      float a = fabsf(buf[i]);
      if (a > peakF) peakF = a;
      if (a > 1.0f) ++over1;
      int s = (int)(buf[i] * 32767);
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      if (s >= 32767 || s <= -32768) { ++clipped; ++winClipNow; }
      uint32_t m = (uint32_t)(s < 0 ? -s : s);
      if (m > peak) peak = m;
      if (wavPath) pcm.push_back((int16_t)s);
    }
    total += got;
    winFrames += got;
    if (winFrames >= (long long)rate * 10) { winClip.push_back(winClipNow); winClipNow = 0; winFrames = 0; }
    remaining -= got;
    if (got < frames) break;
  }
  float minGain = 1.0f, meanRed = 0.0f; uint32_t eng = 0, tot = 0;
  chipnomadTakeLimiterStats(state, &minGain, &meanRed, &eng, &tot);
  std::string names;
  for (auto& e : engines) names += (names.empty() ? "" : ",") + e;
  printf("demo=\"%s\" rate=%d limiter=%d secs=%.1f clip=%lld over1=%lld peak=%.2fdBFS peakF=%.3f tracks=%d engines=[%s] GRmax=%.2fdB GRavg=%.2fdB engaged=%.1f%%\n",
         path, rate, limiter, (double)total / rate, clipped, over1,
         peak ? 20.0 * log10((double)peak / 32768.0) : -999.0, peakF, (int)tracksSeen.size(), names.c_str(),
         minGain < 1.0f ? -20.0 * log10((double)minGain) : 0.0, meanRed > 0.0f ? -20.0 * log10(1.0 - (double)meanRed) : 0.0, tot ? 100.0 * eng / tot : 0.0);
  if (winFrames > 0) winClip.push_back(winClipNow);
  std::string wins;
  for (long long c : winClip) wins += (wins.empty() ? "" : ",") + std::to_string(c);
  printf("  clip_per_10s=[%s]\n", wins.c_str());
  if (wavPath) writeWav(wavPath, pcm, rate);
  chipnomadDestroy(state);
  projectFree(&project);
  return 0;
}
