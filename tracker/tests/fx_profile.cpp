// Offline effect profiler and renderer for the RG Nano FX CPU work.
//   fx_profile micro [--lite=0|1]          per-effect cost (us per 1000 samples)
//   fx_profile master [--lite=0|1]         send reverb / delay cost
//   fx_profile song <file.cct> [seconds] [--lite=N] [--wav=out.wav]
//                                          whole-project time + FX share
//   fx_profile diff a.wav b.wav            peak / RMS / difference of two renders
// Everything runs at 32 kHz (the RG Nano rate) in 1024-frame callbacks.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <pthread.h>
#include <sys/mman.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

#include "audio_monitor.h"
#include "beads_pool.h"
#include "chipnomad_lib.h"
#include "fx_profile.h"
#include "insert_fx.h"
#include "playback.h"
#include "project.h"
#include "screens/screens.h"
#include "synth/master_effects.h"
#include "project_instruments.h"
#include "project_utils.h"

void getSelectionBounds(ScreenData*, int*, int*, int*, int*) {}

namespace {
constexpr int kRate = 32000;
constexpr int kCallback = 1024;
using Clock = std::chrono::steady_clock;

uint32_t rng = 12345;
float noise() {
  rng = rng * 1664525u + 1013904223u;
  return ((rng >> 8) / 8388608.0f) - 1.0f;
}
void fillInput(std::vector<float>& buffer, int frames, bool active, double& phase) {
  for (int i = 0; i < frames; ++i) {
    float l = 0, r = 0;
    if (active) {
      phase += 110.0 / kRate;
      if (phase >= 1) phase -= 1;
      float saw = (float)(phase * 2 - 1) * 0.35f;
      l = saw + noise() * 0.12f;
      r = -saw * 0.8f + noise() * 0.12f;
    }
    buffer[2 * i] = l;
    buffer[2 * i + 1] = r;
  }
}

// Microseconds per 1000 samples for one insert configuration (best of 3).
double timeInsert(int module, const uint8_t* values, bool active) {
  double best = 1e30;
  const bool quick = getenv("FX_QUICK") != nullptr;  // emulated runs: one short repetition
  for (int rep = 0; rep < (quick ? 1 : 3); ++rep) {
    BeadsPool pool;  // outlives the chain that holds its lease
    InsertChain chain(kRate);
    InsertConfig config[2] = {};
    insertSelect(&config[0], module);
    insertSelect(&config[1], insertOff);
    for (int i = 0; i < 8; ++i) config[0].values[i] = insertClamp(module, i, values[i]);
    if (module == insertBeads) chain.attachBeads(0, pool.prepare(0, kRate));
    InsertAutomation automation = {};
    chain.sync(config, &automation, true);
    chain.setTempo(50);
    uint8_t effective[2][8] = {};
    memcpy(effective[0], config[0].values, 8);
    std::vector<float> in(kCallback * 2), work(kCallback * 2);
    double phase = 0;
    // The first second absorbs the switch fade and caches/state setup.
    for (int pass = 0; pass < 2; ++pass) {
      int total = (pass ? (quick ? 1 : 4) : 1) * kRate;
      auto started = Clock::now();
      double spent = 0;
      for (int done = 0; done < total; done += kCallback) {
        fillInput(in, kCallback, active, phase);
        work = in;
        auto t0 = Clock::now();
        chain.process(work.data(), kCallback, effective);
        spent += std::chrono::duration<double>(Clock::now() - t0).count();
      }
      (void)started;
      if (pass) best = std::min(best, spent * 1e6 / (total / 1000.0));
    }
    pool.collect();
  }
  return best;
}

void microRow(const char* label, int module, std::initializer_list<int> vals) {
  uint8_t v[8] = {};
  const auto& d = insertDescriptor(module);
  for (int i = 0; i < 8; ++i) v[i] = d.parameters[i].initial;
  int i = 0;
  for (int x : vals) v[i++] = (uint8_t)x;
  double silent = timeInsert(module, v, false);
  double active = timeInsert(module, v, true);
  std::printf("  %-34s silent %8.1f   active %8.1f\n", label, silent, active);
  std::fflush(stdout);
}

void micro(int only) {
  std::printf("insert FX, us per 1000 samples at %d Hz (x86 -O2; lite=%d)\n", kRate, (int)fxLiteEnabled());
  for (int m = insertCompressor; m < insertModuleCount; ++m) {
    if (only > 0 && m != only) continue;
    const auto& d = insertDescriptor(m);
    std::printf("%s\n", d.name);
    uint8_t def[8] = {}, zero[8] = {}, full[8];
    for (int i = 0; i < 8; ++i) { def[i] = d.parameters[i].initial; full[i] = 255; }
    auto row = [&](const char* label, const uint8_t* v) {
      double s = timeInsert(m, v, false), a = timeInsert(m, v, true);
      std::printf("  %-34s silent %8.1f   active %8.1f\n", label, s, a);
      std::fflush(stdout);
    };
    row("defaults", def);
    row("all params 0", zero);
    row("all params max", full);
    if (m == insertDistortion) {
      for (int mode = 0; mode < 5; ++mode) {
        uint8_t v[8]; memcpy(v, def, 8); v[1] = (uint8_t)mode;
        char label[40]; snprintf(label, sizeof label, "mode %d", mode);
        row(label, v);
      }
    }
    if (m == insertPhaser) {
      uint8_t v[8]; memcpy(v, def, 8); v[4] = 4; row("stages 8", v);
    }
    if (m == insertBeads) {
      static const struct { const char* n; int p[8]; } cases[] = {
        {"density 0", {38, 191, 0, 128, 0, 0, 38, 255}},
        {"density 128 (stopped)", {38, 191, 128, 128, 0, 0, 38, 255}},
        {"density 179 default, mix 100%", {38, 191, 179, 128, 0, 0, 38, 255}},
        {"density 255 (random dense)", {38, 191, 255, 128, 0, 0, 38, 255}},
        {"density 255, size 255 (delay)", {38, 255, 255, 128, 0, 0, 38, 255}},
        {"size 0 (short grains)", {38, 0, 255, 128, 0, 0, 38, 255}},
        {"size 255 long, dens 255", {38, 230, 255, 128, 0, 0, 38, 255}},
        {"feedback 255, dens 179", {38, 191, 179, 128, 0, 255, 38, 255}},
        {"frozen, dens 179", {38, 191, 179, 128, 0, 0, 38 | 128, 255}},
        {"pitch -24", {38, 191, 179, 0, 0, 0, 38, 255}},
        {"pitch +24", {38, 191, 179, 255, 0, 0, 38, 255}},
        {"mix 0 (dry only)", {38, 191, 179, 128, 0, 0, 38, 0}},
      };
      for (auto& c : cases) {
        uint8_t v[8]; for (int i = 0; i < 8; ++i) v[i] = (uint8_t)c.p[i];
        row(c.n, v);
      }
    }
  }
  (void)microRow;
}

void master() {
  std::printf("master sends, us per 1000 samples at %d Hz (x86 -O2; lite=%d)\n", kRate, (int)fxLiteEnabled());
  std::unique_ptr<Project> project(new Project());
  projectInit(project.get());
  for (int algorithm = 0; algorithm < 2; ++algorithm) {
    for (int mode = 0; mode < 3; ++mode) {  // 0 reverb, 1 delay, 2 both
      for (int active = 0; active < 2; ++active) {
        std::unique_ptr<MasterEffects> fx(new MasterEffects());
        fx->init(kRate);
        fx->reserve(kCallback);
        project->reverbAlgorithm = (uint8_t)algorithm;
        project->tickRate = 50;
        project->delayReturn = 100;
        project->delayFeedback = 60;
        std::vector<float> rev(kCallback * 2), del(kCallback * 2), out(kCallback * 2);
        double phase = 0, spent = 0;
        int total = 5 * kRate;
        for (int done = 0; done < total; done += kCallback) {
          fillInput(rev, kCallback, active, phase);
          del = rev;
          std::fill(out.begin(), out.end(), 0.0f);
          float* r = (mode == 1) ? nullptr : rev.data();
          float* d = (mode == 0) ? nullptr : del.data();
          // The first second warms; only later callbacks are timed.
          auto t0 = Clock::now();
          fx->process(r, d, out.data(), kCallback, project.get());
          if (done >= kRate) spent += std::chrono::duration<double>(Clock::now() - t0).count();
        }
        std::printf("  %-8s %-7s input %-7s %8.1f\n", algorithm ? "beadsRev" : "clouds",
                    mode == 0 ? "reverb" : mode == 1 ? "delay" : "both", active ? "active" : "silent",
                    spent * 1e6 / ((total - kRate) / 1000.0));
      }
    }
  }
}


// Stack depth of the audio callback: renders on a thread whose stack is painted
// beforehand (the RG Nano audio thread is an SDL thread on musl: 128 KiB).
struct StackJob { ChipNomadState* state; int frames; };
void* stackThread(void* arg) {
  auto* job = (StackJob*)arg;
  std::vector<float> buffer(kCallback * 2);
  for (int done = 0; done < job->frames; done += kCallback) chipnomadRender(job->state, buffer.data(), kCallback);
  return nullptr;
}
int stackProbe(int track, int beadsSlots, int seconds) {
  std::unique_ptr<Project> project(new Project());
  projectInit(project.get());
  project->chipsCount = 1;
  project->tracksCount = 1;
  project->tickRate = 50;
  phraseClear(&project->phrases[0]);
  Instrument* ins = &project->instruments[0];
  ins->type = (InstrumentType)track;
  getInstrumentFunctions(ins->type).init(ins);
  project->song[0][0] = 0;
  project->chains[0].rows[0].phrase = 0;
  project->phrases[0].rows[0].note = 48;
  project->phrases[0].rows[0].instrument = 0;
  project->phrases[0].rows[0].volume = 100;
  for (int slot = 0; slot < beadsSlots && slot < 2; ++slot) insertSelect(&project->trackInserts[0][slot], insertBeads);
  ChipNomadState* state = chipnomadCreate();
  state->ownsProjectResources = 0;
  state->project = *project;
  state->audioProject = *project;
  playbackInit(&state->playbackState, &state->audioProject);
  chipnomadInitChips(state, kRate, nullptr);
  chipnomadReserveRenderBuffers(state, kCallback);
  playbackStartSong(&state->playbackState, 0, 0, 1);
  const size_t size = 512 * 1024;
  void* stack = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  memset(stack, 0xA5, size);
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstack(&attr, stack, size);
  StackJob job{state, seconds * kRate};
  pthread_t thread;
  pthread_create(&thread, &attr, stackThread, &job);
  pthread_join(thread, nullptr);
  const unsigned char* bytes = (const unsigned char*)stack;
  size_t untouched = 0;
  while (untouched < size && bytes[untouched] == 0xA5) ++untouched;
  std::printf("instrument type %d, Beads slots %d: audio-thread stack used %zu bytes (x86-64, -O2)\n", track, beadsSlots,
              size - untouched);
  chipnomadDestroy(state);
  return 0;
}


// UI-style edit flow for the Beads crash hunt: Beads selected in both lanes of
// a Plaits track while stopped, playback started, stopped, restarted, lanes
// swapped. Run it under AddressSanitizer / the allocation trap.
int beadsFlow(int track, int density) {
  std::unique_ptr<Project> project(new Project());
  projectInit(project.get());
  project->chipsCount = 1;
  project->tracksCount = 1;
  project->tickRate = 50;
  phraseClear(&project->phrases[0]);
  Instrument* ins = &project->instruments[0];
  ins->type = (InstrumentType)track;
  getInstrumentFunctions(ins->type).init(ins);
  project->song[0][0] = 0;
  project->chains[0].rows[0].phrase = 0;
  project->phrases[0].rows[0].note = 48;
  project->phrases[0].rows[0].instrument = 0;
  project->phrases[0].rows[0].volume = 100;
  ChipNomadState* state = chipnomadCreate();
  state->ownsProjectResources = 0;
  state->project = *project;
  state->audioProject = *project;
  playbackInit(&state->playbackState, &state->audioProject);
  chipnomadInitChips(state, kRate, nullptr);
  chipnomadReserveRenderBuffers(state, kCallback);
  std::vector<float> buffer(kCallback * 2);
  double spent = 0;
  long frames = 0;
  auto render = [&](int callbacks) {
    float peak = 0;
    for (int i = 0; i < callbacks; ++i) {
      auto t0 = Clock::now();
      chipnomadRender(state, buffer.data(), kCallback);
      if (state->playbackState.tracks[0].mode != PlaybackMode::stopped) {
        spent += std::chrono::duration<double>(Clock::now() - t0).count();
        frames += kCallback;
      }
      chipnomadGetPlaybackStatus(state);  // UI side of the lazy voice builder
      for (float x : buffer) peak = std::max(peak, fabsf(x));
    }
    return peak;
  };
  std::printf("stopped, no inserts: peak %.3f\n", render(10));
  insertSelect(&state->project.trackInserts[0][0], insertBeads);
  insertSelect(&state->project.trackInserts[0][1], insertBeads);
  if (density >= 0)
    for (int lane = 0; lane < 2; ++lane) {
      state->project.trackInserts[0][lane].values[2] = (uint8_t)density;
      state->project.trackInserts[0][lane].values[1] = 255;
    }
  std::printf("refresh with Beads in both lanes: %d\n", chipnomadQueueProjectRefresh(state));
  std::printf("stopped, applied: peak %.3f\n", render(10));
  for (int round = 0; round < 3; ++round) {
    chipnomadQueuePlaybackStartSong(state, 0, 0, 1);
    std::printf("round %d playing: peak %.3f\n", round, render(60));
    chipnomadQueuePlaybackStop(state);
    render(20);
  }
  insertSelect(&state->project.trackInserts[0][0], insertOff);
  chipnomadQueueProjectRefresh(state);
  render(40);
  insertSelect(&state->project.trackInserts[0][0], insertBeads);
  chipnomadQueueProjectRefresh(state);
  chipnomadQueuePlaybackStartSong(state, 0, 0, 1);
  std::printf("lane swap then play: peak %.3f\n", render(60));
  std::printf("playing cost: %.1f us per 1000 samples = %.1f%% of a real-time core on this x86 (A7 ESTIMATE at x11: %.0f%%)\n",
              spent * 1e6 / (frames / 1000.0), spent * 100.0 / (frames / (double)kRate),
              spent * 100.0 / (frames / (double)kRate) * 11);
  FxProfile fp;
  chipnomadGetFxProfile(&fp, true);
  char line[200];
  fxProfileFormat(fp, (int)(frames / kCallback), line, sizeof line);
  std::printf("fx us/cb: %s\n", line);
  chipnomadDestroy(state);
  return 0;
}


// Where does the mix get hot? Renders a project several ways and reports peaks:
// master sum (before the limiter), per-track post-insert peaks from the monitor,
// the same with every insert removed, and each track soloed.
struct PeakRun {
  double master = 0;
  long over = 0, frames = 0;
  double track[PROJECT_MAX_TRACKS] = {};
  float inPeak[PROJECT_MAX_TRACKS][2] = {}, outPeak[PROJECT_MAX_TRACKS][2] = {};
  int module[PROJECT_MAX_TRACKS][2] = {};
};
PeakRun peakRender(Project* project, int seconds, int solo, bool noInserts, double* rmsOut = nullptr) {
  Project copy = *project;
  if (noInserts) {
    for (auto& inserts : copy.trackInserts) for (auto& insert : inserts) insert.module = insertOff;
    for (auto& insert : copy.masterInserts) insert.module = insertOff;
  }
  ChipNomadState* state = chipnomadCreate();
  state->ownsProjectResources = 0;
  state->project = copy;
  state->audioProject = copy;
  playbackInit(&state->playbackState, &state->audioProject);
  chipnomadInitChips(state, kRate, nullptr);
  chipnomadReserveRenderBuffers(state, kCallback);
  if (solo >= 0) for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) state->playbackState.trackEnabled[t] = t == solo;
  playbackStartSong(&state->playbackState, 0, 0, 1);
  std::vector<float> buffer(kCallback * 2);
  PeakRun run;
  double sum = 0;
  for (int done = 0; done < seconds * kRate; done += kCallback) {
    if (chipnomadRender(state, buffer.data(), kCallback) != kCallback) break;
    for (float x : buffer) {
      run.master = std::max(run.master, (double)fabsf(x));
      run.over += fabsf(x) > 1.0f;
      sum += (double)x * x;
    }
    run.frames += kCallback;
    for (int t = 0; t < PROJECT_MAX_TRACKS; ++t)
      for (int slot = 0; slot < 2; ++slot)
        if (state->insertChains[t]) {
          float in, out;
          state->insertChains[t]->peaks(slot, &in, &out, true);
          run.inPeak[t][slot] = std::max(run.inPeak[t][slot], in);
          run.outPeak[t][slot] = std::max(run.outPeak[t][slot], out);
          run.module[t][slot] = copy.trackInserts[t][slot].module;
        }
    AudioMonitorSnapshot snap;
    if (state->audioMonitor->receive(snap))
      for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) run.track[t] = std::max(run.track[t], (double)std::max(snap.peaks[t][0], snap.peaks[t][1]));
  }
  if (rmsOut) *rmsOut = sqrt(sum / std::max<long>(1, run.frames * 2));
  chipnomadDestroy(state);
  return run;
}
int peaks(const char* path, int seconds) {
  Project project;
  projectInit(&project);
  if (projectLoad(&project, path)) { std::fprintf(stderr, "Cannot load %s\n", path); return 1; }
  auto db = [](double v) { return v > 0 ? 20 * log10(v) : -999.0; };
  std::printf("project %s, mixVolume trim not applied here (raw engine floats)\n", path);
  for (int noInserts = 0; noInserts < 2; ++noInserts) {
    double rms;
    PeakRun r = peakRender(&project, seconds, -1, noInserts != 0, &rms);
    std::printf("%s: master peak %+.2f dBFS (%.4f), rms %.2f dBFS, samples over 1.0: %ld of %ld (%.2f%%)\n",
                noInserts ? "inserts removed" : "as authored", db(r.master), r.master, db(rms), r.over, r.frames * 2,
                100.0 * r.over / (r.frames * 2));
    for (int t = 0; t < project.tracksCount && !noInserts; ++t)
      for (int slot = 0; slot < 2; ++slot)
        if (r.module[t][slot])
          std::printf("   T%d insert %d %-10s in %+6.1f dBFS  out %+6.1f dBFS\n", t, slot, insertDescriptor(r.module[t][slot]).name,
                      db(r.inPeak[t][slot]), db(r.outPeak[t][slot]));
    std::printf("   per-track peak (monitor, after volume/pan/tilt/inserts):");
    for (int t = 0; t < project.tracksCount; ++t) std::printf(" T%d %+.1f", t, db(r.track[t]));
    std::printf(" dBFS\n");
  }
  std::printf("each track soloed (master peak after its inserts and sends):\n");
  for (int t = 0; t < project.tracksCount; ++t) {
    double rms;
    PeakRun r = peakRender(&project, seconds, t, false, &rms);
    PeakRun clean = peakRender(&project, seconds, t, true);
    std::printf("   T%d: %+.2f dBFS  (inserts removed %+.2f dBFS)  over-1.0 samples %ld\n", t, db(r.master), db(clean.master), r.over);
  }
  projectFree(&project);
  return 0;
}


// Beads full engine vs FX lite on the same input: level and octave-band energy
// of the wet signal. Granular output is randomised, so waveforms differ by
// design; long-term level and spectrum are what must stay close.
void spectrum(const std::vector<float>& mono, double bands[9], double* rms, double* peak) {
  const int n = 2048;
  std::vector<double> power(n / 2, 0.0);
  int frames = 0;
  for (size_t start = 0; start + n <= mono.size(); start += n / 2) {
    std::vector<double> re(n), im(n, 0.0);
    for (int i = 0; i < n; ++i) re[i] = mono[start + i] * (0.5 - 0.5 * cos(2 * M_PI * i / n));
    for (int len = n; len > 1; len >>= 1) {}
    // iterative radix-2 FFT
    for (int i = 1, j = 0; i < n; ++i) {
      int bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= n; len <<= 1) {
      double ang = -2 * M_PI / len;
      for (int i = 0; i < n; i += len)
        for (int k = 0; k < len / 2; ++k) {
          double wr = cos(ang * k), wi = sin(ang * k);
          double ur = re[i + k], ui = im[i + k];
          double vr = re[i + k + len / 2] * wr - im[i + k + len / 2] * wi;
          double vi = re[i + k + len / 2] * wi + im[i + k + len / 2] * wr;
          re[i + k] = ur + vr; im[i + k] = ui + vi;
          re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        }
    }
    for (int i = 0; i < n / 2; ++i) power[i] += re[i] * re[i] + im[i] * im[i];
    ++frames;
  }
  static const double edges[10] = {0, 125, 250, 500, 1000, 2000, 4000, 8000, 12000, 16000};
  for (int b = 0; b < 9; ++b) {
    double sum = 0;
    for (int i = 0; i < n / 2; ++i) {
      double hz = i * (double)kRate / n;
      if (hz >= edges[b] && hz < edges[b + 1]) sum += power[i];
    }
    bands[b] = 10 * log10(sum / std::max(1, frames) + 1e-30);
  }
  double sum = 0, pk = 0;
  for (float x : mono) { sum += (double)x * x; pk = std::max(pk, (double)fabsf(x)); }
  *rms = 10 * log10(sum / mono.size() + 1e-30);
  *peak = 20 * log10(pk + 1e-30);
}
int beadsCompare() {
  struct Case { const char* name; int p[8]; };
  static const Case cases[] = {
    {"default (dens 179)", {38, 191, 179, 128, 0, 0, 38, 255}},
    {"dense regular (dens 0)", {38, 191, 0, 128, 0, 0, 38, 255}},
    {"dense random (dens 255)", {38, 191, 255, 128, 0, 0, 38, 255}},
    {"short grains dens 255", {38, 0, 255, 128, 0, 0, 38, 255}},
    {"long grains dens 255", {38, 230, 255, 128, 0, 0, 38, 255}},
    {"pitch +12 st", {38, 191, 179, 192, 0, 0, 38, 255}},
    {"pitch -12 st", {38, 191, 179, 64, 0, 0, 38, 255}},
    {"pitch +24 st", {38, 191, 179, 255, 0, 0, 38, 255}},
    {"feedback 60%", {38, 191, 179, 128, 0, 153, 38, 255}},
    {"delay mode (size 255)", {38, 255, 128, 128, 0, 0, 38, 255}},
  };
  std::printf("%-26s %7s %7s %7s | octave-band deltas lite-full dB (125-250-500-1k-2k-4k-8k-12k-16k Hz upper edges)\n",
              "case", "dRMS", "dPeak", "dBand~");
  for (const auto& c : cases) {
    std::vector<float> out[2];
    for (int lite = 0; lite < 2; ++lite) {
      fxLiteSet(lite != 0);
      BeadsPool pool;
      InsertChain chain(kRate);
      InsertConfig config[2] = {};
      insertSelect(&config[0], insertBeads);
      insertSelect(&config[1], insertOff);
      for (int i = 0; i < 8; ++i) config[0].values[i] = insertClamp(insertBeads, i, c.p[i]);
      chain.attachBeads(0, pool.prepare(0, kRate));
      InsertAutomation automation = {};
      chain.sync(config, &automation, true);
      uint8_t effective[2][8] = {};
      memcpy(effective[0], config[0].values, 8);
      std::vector<float> in(kCallback * 2), work(kCallback * 2);
      double phase = 0;
      rng = 777;
      for (int done = 0; done < 10 * kRate; done += kCallback) {
        for (int i = 0; i < kCallback; ++i) {
          int t = done + i;
          float env = (t % 8000) < 2400 ? expf(-(t % 8000) / 900.0f) : 0.05f;
          phase += (110.0 + 55.0 * ((t / 16000) % 3)) / kRate;
          if (phase >= 1) phase -= 1;
          float saw = (float)(phase * 2 - 1);
          in[2 * i] = (saw * 0.35f + noise() * 0.1f) * env;
          in[2 * i + 1] = (saw * -0.3f + noise() * 0.1f) * env;
        }
        work = in;
        chain.process(work.data(), kCallback, effective);
        if (done >= 3 * kRate)
          for (int i = 0; i < kCallback; ++i) out[lite].push_back(0.5f * (work[2 * i] + work[2 * i + 1]));
      }
    }
    double bandsA[9], bandsB[9], rmsA, rmsB, peakA, peakB;
    spectrum(out[0], bandsA, &rmsA, &peakA);
    spectrum(out[1], bandsB, &rmsB, &peakB);
    double worst = 0;
    for (int b = 0; b < 9; ++b) if (bandsA[b] > rmsA + 10 * log10(1e-3) - 30 && fabs(bandsB[b] - bandsA[b]) > fabs(worst)) worst = bandsB[b] - bandsA[b];
    std::printf("%-26s %+6.2f %+6.2f %+6.2f |", c.name, rmsB - rmsA, peakB - peakA, worst);
    for (int b = 0; b < 9; ++b) std::printf(" %+5.1f", bandsB[b] - bandsA[b]);
    std::printf("\n%-26s full abs dB re total:", "");
    for (int b = 0; b < 9; ++b) std::printf(" %+6.1f", bandsA[b] - 10 * log10(std::max(1e-30, pow(10, bandsA[0] / 10) + pow(10, bandsA[1] / 10) + pow(10, bandsA[2] / 10) + pow(10, bandsA[3] / 10) + pow(10, bandsA[4] / 10) + pow(10, bandsA[5] / 10) + pow(10, bandsA[6] / 10) + pow(10, bandsA[7] / 10) + pow(10, bandsA[8] / 10))));
    std::printf("\n");
  }
  fxLiteSet(false);
  return 0;
}

struct Wav {
  std::vector<int16_t> pcm;
};
void writeWav(const char* path, const std::vector<float>& data) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  uint32_t bytes = (uint32_t)data.size() * 2, rate = kRate, byteRate = kRate * 4, riff = 36 + bytes;
  uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
  uint32_t sixteen = 16;
  fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
  fwrite(&sixteen, 4, 1, f); fwrite(&fmt, 2, 1, f); fwrite(&ch, 2, 1, f);
  fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
  for (float x : data) {
    float c = std::max(-1.0f, std::min(1.0f, x));
    int16_t s = (int16_t)lrintf(c * 32767.0f);
    fwrite(&s, 2, 1, f);
  }
  fclose(f);
}
std::vector<float> readWav(const char* path) {
  std::vector<float> out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  fseek(f, 44, SEEK_SET);
  int16_t s;
  while (fread(&s, 2, 1, f) == 1) out.push_back(s / 32768.0f);
  fclose(f);
  return out;
}

int song(const char* path, int seconds, const char* wavPath) {
  Project project;
  projectInit(&project);
  if (projectLoad(&project, path)) {
    std::fprintf(stderr, "Cannot load %s: %s\n", path, projectFileError);
    return 1;
  }
  ChipNomadState* state = chipnomadCreate();
  if (!state) return 1;
  state->ownsProjectResources = 0;
  state->project = project;
  state->audioProject = project;
  playbackInit(&state->playbackState, &state->audioProject);
  chipnomadInitChips(state, kRate, nullptr);
  chipnomadReserveRenderBuffers(state, kCallback);
  playbackStartSong(&state->playbackState, 0, 0, 1);
  std::vector<float> buffer(kCallback * 2), all;
  int remaining = seconds * kRate;
  double spent = 0;
  int warm = 2 * kRate;  // first two seconds: lazy voice builds, not steady state
  int measured = 0;
  FxProfile profile;
  while (remaining > 0) {
    int frames = std::min(remaining, kCallback);
    if (warm > 0 && wavPath == nullptr) {
      // keep warmup out of the profile
    }
    auto t0 = Clock::now();
    int got = chipnomadRender(state, buffer.data(), frames);
    double dt = std::chrono::duration<double>(Clock::now() - t0).count();
    if (got != frames) break;
    if (warm > 0) {
      warm -= frames;
      if (warm <= 0) chipnomadGetFxProfile(&profile, true);
    } else {
      spent += dt;
      measured += frames;
    }
    if (wavPath) all.insert(all.end(), buffer.begin(), buffer.begin() + frames * 2);
    remaining -= frames;
  }
  chipnomadGetFxProfile(&profile, false);
  double audioSeconds = measured / (double)kRate;
  if (audioSeconds > 0) {
    std::printf("project %s  lite=%d\n  measured %.1f s of audio after 2 s warmup\n  total render %.1f us/1000 samples (%.1f%% of one core on x86)\n",
                path, (int)fxLiteEnabled(), audioSeconds, spent * 1e6 / (measured / 1000.0),
                spent * 100.0 / audioSeconds);
    double fxTotal = 0;
    for (int m = 0; m < FxProfile::insertSlots; ++m) {
      if (!profile.insertNs[m]) continue;
      double us = profile.insertNs[m] / 1e3 / (measured / 1000.0);
      fxTotal += us;
      std::printf("  insert %-14s %8.1f us/1000 samples (%.1f%% of render)\n",
                  insertDescriptor(m).name, us, us / (spent * 1e6 / (measured / 1000.0)) * 100.0);
    }
    double rev = profile.reverbNs / 1e3 / (measured / 1000.0), brev = profile.beadsReverbNs / 1e3 / (measured / 1000.0),
           del = profile.delayNs / 1e3 / (measured / 1000.0);
    fxTotal += rev + brev + del;
    std::printf("  clouds reverb send   %8.1f\n  beads reverb send    %8.1f\n  delay send           %8.1f\n", rev, brev, del);
    double all_us = spent * 1e6 / (measured / 1000.0);
    std::printf("  FX total             %8.1f us/1000 samples = %.1f%% of render; voices+mix+rest %.1f\n",
                fxTotal, fxTotal / all_us * 100.0, all_us - fxTotal);
    std::printf("  insert frames skipped by silence bypass: %llu of %llu\n",
                (unsigned long long)profile.skippedFrames,
                (unsigned long long)(profile.skippedFrames + profile.processedFrames));
  }
  if (wavPath) writeWav(wavPath, all);
  chipnomadDestroy(state);
  projectFree(&project);
  return 0;
}

int diff(const char* a, const char* b) {
  auto x = readWav(a), y = readWav(b);
  size_t n = std::min(x.size(), y.size());
  double sa = 0, sb = 0, sd = 0, pa = 0, pb = 0, pd = 0;
  for (size_t i = 0; i < n; ++i) {
    sa += x[i] * x[i]; sb += y[i] * y[i]; sd += (x[i] - y[i]) * (x[i] - y[i]);
    pa = std::max(pa, (double)fabs(x[i])); pb = std::max(pb, (double)fabs(y[i])); pd = std::max(pd, (double)fabs(x[i] - y[i]));
  }
  auto db = [](double v) { return v > 0 ? 20 * log10(v) : -999.0; };
  std::printf("samples %zu vs %zu (compared %zu)%s\n", x.size(), y.size(), n, sd == 0 && x.size() == y.size() ? "  BIT-IDENTICAL" : "");
  std::printf("A peak %.2f dBFS rms %.2f dBFS | B peak %.2f dBFS rms %.2f dBFS\n", db(pa), db(sqrt(sa / n)), db(pb), db(sqrt(sb / n)));
  std::printf("delta peak %+.2f dB, delta rms %+.2f dB | diff peak %.2f dBFS, diff rms %.2f dBFS (SNR %.1f dB)\n",
              db(pb) - db(pa), db(sqrt(sb / n)) - db(sqrt(sa / n)), db(pd), db(sqrt(sd / n)),
              sd > 0 ? db(sqrt(sa / n)) - db(sqrt(sd / n)) : 999.0);
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 1;
#if defined(__SSE__)
  // The device audio thread runs with ARM FPSCR.FZ set; match it here.
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__arm__)
  { uint32_t fpscr; __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr)); fpscr |= (1u << 24); __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr)); }
#endif
  const char* wav = nullptr;
  std::vector<const char*> args;
  for (int i = 1; i < argc; ++i) {
    if (!strncmp(argv[i], "--lite=", 7)) fxLiteSet(atoi(argv[i] + 7) != 0);
    else if (!strncmp(argv[i], "--wav=", 6)) wav = argv[i] + 6;
    else args.push_back(argv[i]);
  }
  std::string cmd = args[0];
  if (cmd == "micro") { micro(args.size() > 1 ? atoi(args[1]) : 0); return 0; }
  if (cmd == "master") { master(); return 0; }
  if (cmd == "song" && args.size() >= 2) return song(args[1], args.size() > 2 ? atoi(args[2]) : 20, wav);
  if (cmd == "stack" && args.size() >= 3) return stackProbe(atoi(args[1]), atoi(args[2]), 4);
  if (cmd == "peaks" && args.size() >= 2) return peaks(args[1], args.size() > 2 ? atoi(args[2]) : 30);
  if (cmd == "beadscmp") return beadsCompare();
  if (cmd == "beadsflow") return beadsFlow(args.size() > 1 ? atoi(args[1]) : 6, args.size() > 2 ? atoi(args[2]) : -1);
  if (cmd == "diff" && args.size() >= 3) return diff(args[1], args[2]);
  return 1;
}
