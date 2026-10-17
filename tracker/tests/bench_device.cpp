// On-device cost battery for the RG Nano (Cortex-A7) port. Offline: renders at
// 32 kHz in 512-frame callbacks (the app's audio block) with no audio device.
//
//   bench_device [--secs N] [--warm N] [--log PATH] [--est PATH] [--assets DIR]
//                [--passes fifo,normal] [--only SUBSTR] [--host-est OUT] [--no-fb]
//
// Every item runs in a forked child (an out-of-memory kill only loses that
// item) and renders N seconds (default 5). Cost is render time / audio time:
// mean and worst block, in percent of real time. Pass "fifo" runs the child at
// SCHED_FIFO 70 like the audio thread; pass "normal" runs it SCHED_OTHER.
//   --host-est OUT   run on a PC (x86): write per-item mean % x11 (the A7
//                    estimate fx_profile uses) to OUT as the --est table.
//   --est PATH       the table above; the summary prints real / estimate.
// Items: every synth engine at 1 and 4 voices, every insert FX full / lite /
// silence-bypassed, the master chain (limiter, Clouds reverb, Beads reverb, delay).
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fb.h>
#endif
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#include "beads_pool.h"
#include "chipnomad_lib.h"
#include "fx_profile.h"
#include "insert_fx.h"
#include "playback.h"
#include "project.h"
#include "project_instruments.h"
#include "project_utils.h"
#include "screens/screens.h"
#include "synth/sample_voice.h"
#include "synth/sr_wavetable_loader.h"

void getSelectionBounds(ScreenData*, int*, int*, int*, int*) {}

namespace {
constexpr int kRate = 32000;
constexpr int kBlock = 512;
constexpr double kBlockSec = (double)kBlock / kRate;
constexpr double kA7Factor = 11.0;  // x86 -> A7 estimate used by tests/fx_profile.cpp

int gSecs = 5, gWarm = 1, gMaxSecs = 50 * 60;  // stop starting new items after 50 min
std::string gAssets = ".";

double nowSec() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}
void enableFtz() {
#if defined(__SSE__)
  _mm_setcsr(_mm_getcsr() | 0x8040);
#elif defined(__arm__)
  unsigned fp;
  __asm__ volatile("vmrs %0, fpscr" : "=r"(fp));
  fp |= (1u << 24);
  __asm__ volatile("vmsr fpscr, %0" ::"r"(fp));
#endif
}
long readFreqKhz() {
  FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
  if (!f) return -1;
  long v = -1;
  if (fscanf(f, "%ld", &v) != 1) v = -1;
  fclose(f);
  return v;
}

enum Kind { kEngine, kInsert, kMaster };
struct Item {
  Kind kind;
  std::string name;
  int type;       // InstrumentType (engine/master) or InsertModule
  int voices;     // engine voice count
  int variant;    // insert: 0 full, 1 lite, 2 silent+lite; master: see runItem
  bool dense;
  std::string key() const {
    char b[160];
    snprintf(b, sizeof b, "%s|%s|%d|%d", kind == kEngine ? "engine" : kind == kInsert ? "insert" : "master", name.c_str(),
             voices, variant);
    return b;
  }
};

struct Result {
  double mean, max, p99;
  int blocks;
  double skipPct;
  long freqKhz;
  int ok;  // 1 ok, 2 ok but SCHED_FIFO was refused
};

struct Timing {
  std::vector<double> pct;
  void add(double seconds) { pct.push_back(seconds / kBlockSec * 100.0); }
  void finish(Result* r) {
    r->blocks = (int)pct.size();
    if (pct.empty()) { r->mean = r->max = r->p99 = 0; return; }
    double sum = 0, mx = 0;
    for (double v : pct) { sum += v; mx = std::max(mx, v); }
    std::sort(pct.begin(), pct.end());
    r->mean = sum / pct.size();
    r->max = mx;
    r->p99 = pct[std::min(pct.size() - 1, (size_t)(pct.size() * 0.99))];
  }
};

// Yield the core briefly so a SCHED_FIFO child cannot starve the parent (progress
// screen) or the kernel for a whole item. The sleep is outside the timed region.
void breathe(int blockIndex) {
  if ((blockIndex & 15) == 15) {
    timespec ts = {0, 2 * 1000 * 1000};
    nanosleep(&ts, nullptr);
  }
}

std::string assetPath(const char* rel) {
  std::string a = gAssets + "/" + rel;
  if (access(a.c_str(), R_OK) == 0) return a;
  std::string b = gAssets + "/packaging/common/" + rel;
  if (access(b.c_str(), R_OK) == 0) return b;
  return a;
}

struct MasterCfg { int limiter, reverb, delay, revAlg; };

bool runRender(const Item& it, const MasterCfg& mc, Result* res) {
  ChipNomadState* s = chipnomadCreate();
  if (!s) return false;
  Project& p = s->project;
  const std::string demo = assetPath("projects/gm-midi-demo.cct");
  if (projectLoad(&p, demo.c_str())) { fprintf(stderr, "cannot load %s\n", demo.c_str()); return false; }
  const int tracks = it.voices;
  for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) p.trackVolume[t] = 100;
  p.chipsCount = std::max(1, tracks);
  p.tracksCount = projectGetTotalTracks(&p);
  for (int t = 0; t < 8; ++t) {
    p.song[0][t] = EMPTY_VALUE_16;
    p.trackTilt[t] = 0x80;
    p.trackReverbSend[t] = mc.reverb ? 60 : 0;
    p.trackDelaySend[t] = mc.delay ? 60 : 0;
    insertSelect(&p.trackInserts[t][0], insertOff);
    insertSelect(&p.trackInserts[t][1], insertOff);
  }
  p.reverbAlgorithm = (uint8_t)mc.revAlg;
  const InstrumentType type = (InstrumentType)it.type;
  for (int t = 0; t < tracks; ++t) {
    Instrument& in = p.instruments[t];
    if (type != InstrumentType::none) {
      getInstrumentFunctions(type).init(&in);
      char err[128];
      if (type == InstrumentType::Sample) {
        const std::string w = assetPath("samples/ST-01/strings6.wav");
        if (sampleLoadWav16(w.c_str(), &in.chip.sample, err, sizeof err)) { fprintf(stderr, "wav: %s\n", err); return false; }
        in.chip.sample.loopMode = 1;
      }
      if (type == InstrumentType::SCWF || type == InstrumentType::BYOWTBL) {
        const std::string w = assetPath("waveforms/AKWF/AKWF_cello_0001.wav");
        auto& sc = in.chip.scwf;
        for (int o = 0; o < 2; ++o)
          if (sampleLoadWav16(w.c_str(), &sc.oscillator[o], err, sizeof err)) { fprintf(stderr, "scwf: %s\n", err); return false; }
        if (type == InstrumentType::BYOWTBL) {
          const std::string sw = assetPath("SR_wavetables/WaveEdit/PPG_WA04.WAV");
          auto& bw = in.chip.byowtbl;
          for (int o = 0; o < 2; ++o)
            if (srWavetableLoadWav(sw.c_str(), &bw.oscillator[o], &bw.frameSize[o], &bw.tableFrames[o], err, sizeof err)) {
              fprintf(stderr, "sr wav: %s\n", err);
              return false;
            }
        }
      }
      if (type == InstrumentType::AYSample) {
        auto& as = in.chip.aySample;
        uint8_t* d = (uint8_t*)malloc(4096);
        for (int i = 0; i < 4096; ++i) d[i] = (i * 7) & 255;
        as.sampleData = d; as.fileLength = 4096; as.sampleLength = 4096; as.sampleRate = 8000;
        as.sampleStart = 0; as.sampleLoopStart = 0;
      }
    }
    p.song[0][t] = t;
    p.chains[t].rows[0].phrase = t;
    p.chains[t].rows[0].transpose = 0;
    phraseClear(&p.phrases[t]);
    if (type != InstrumentType::none)
      for (int row = 0; row < 16; row += (it.dense ? 4 : 16)) {
        auto& r = p.phrases[t].rows[row];
        r.note = 48 + t * 2; r.instrument = t; r.volume = 100;
      }
  }
  s->audioProject = s->project;
  playbackInit(&s->playbackState, &s->audioProject);
  chipnomadInitChips(s, kRate, nullptr);
  chipnomadReserveRenderBuffers(s, kBlock);
  chipnomadSetQuality(s, (ChipNomadQuality)1);
  chipnomadSetMasterLimiter(s, mc.limiter);
  playbackStartSong(&s->playbackState, 0, 0, 1);
  std::vector<float> buf(kBlock * 2 + 16);
  const int warm = gWarm * kRate / kBlock, meas = gSecs * kRate / kBlock;
  Timing tm;
  double t0all = nowSec();
  for (int b = 0; b < warm + meas; ++b) {
    double t0 = nowSec();
    int rr = chipnomadRender(s, buf.data(), kBlock);
    double dt = nowSec() - t0;
    if (rr != kBlock) { fprintf(stderr, "render short %d at block %d\n", rr, b); return false; }
    if (b >= warm) tm.add(dt);
    breathe(b);
    if (nowSec() - t0all > 8.0 * (gSecs + gWarm) + 10) { fprintf(stderr, "item too slow, aborted\n"); break; }
  }
  res->freqKhz = readFreqKhz();
  tm.finish(res);
  res->skipPct = 0;
  chipnomadDestroy(s);
  return true;
}

uint32_t rng = 12345;
float noise() {
  rng = rng * 1664525u + 1013904223u;
  return ((rng >> 8) / 8388608.0f) - 1.0f;
}
void fillInput(std::vector<float>& buffer, bool active, double& phase) {
  for (int i = 0; i < kBlock; ++i) {
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

bool runInsert(const Item& it, Result* res) {
  const int module = it.type;
  const bool lite = it.variant != 0, active = it.variant != 2;
  fxLiteSet(lite);
  BeadsPool pool;
  InsertChain chain(kRate);
  InsertConfig config[2] = {};
  insertSelect(&config[0], module);
  insertSelect(&config[1], insertOff);
  const auto& d = insertDescriptor(module);
  for (int i = 0; i < 8; ++i) config[0].values[i] = insertClamp(module, i, d.parameters[i].initial);
  if (module == insertBeads) chain.attachBeads(0, pool.prepare(0, kRate));
  InsertAutomation automation = {};
  chain.sync(config, &automation, true);
  chain.setTempo(50);
  chain.setRealtime(false);  // never let the governor bypass the slot under test
  uint8_t effective[2][8] = {};
  memcpy(effective[0], config[0].values, 8);
  std::vector<float> in(kBlock * 2), work(kBlock * 2);
  double phase = 0;
  // Silent case: the lite bypass needs the tail to decay first, so warm up longer.
  const int warmSecs = active ? gWarm : 3;
  const int warm = warmSecs * kRate / kBlock, meas = gSecs * kRate / kBlock;
  FxProfile after;
  Timing tm;
  for (int b = 0; b < warm + meas; ++b) {
    fillInput(in, active, phase);
    work = in;
    if (b == warm) chipnomadGetFxProfile(&after, true);  // restart the skip counters
    double t0 = nowSec();
    chain.process(work.data(), kBlock, effective);
    double dt = nowSec() - t0;
    if (b >= warm) tm.add(dt);
    breathe(b);
  }
  chipnomadGetFxProfile(&after, false);
  const uint64_t frames = after.skippedFrames + after.processedFrames;
  res->skipPct = frames ? 100.0 * after.skippedFrames / frames : 0;
  res->freqKhz = readFreqKhz();
  tm.finish(res);
  pool.collect();
  return true;
}

bool runItem(const Item& it, Result* res) {
  memset(res, 0, sizeof *res);
  enableFtz();
  bool ok = false;
  if (it.kind == kInsert) ok = runInsert(it, res);
  else if (it.kind == kEngine) ok = runRender(it, MasterCfg{0, 0, 0, 0}, res);
  else {
    // variant: 0 limiter off, 1 limiter on, 2 Clouds reverb send, 3 Beads reverb send, 4 delay send
    MasterCfg mc{it.variant == 1, it.variant == 2 || it.variant == 3, it.variant == 4, it.variant == 3};
    ok = runRender(it, mc, res);
  }
  res->ok = ok ? 1 : 0;
  return ok;
}

std::vector<Item> buildItems() {
  std::vector<Item> v;
  v.push_back({kEngine, "BASELINE-idle", (int)InstrumentType::none, 1, 0, false});
  struct E { const char* n; InstrumentType t; bool dense; } eng[] = {
      {"Sample(PCM)", InstrumentType::Sample, false}, {"DX7", InstrumentType::DX7, false},
      {"Braids", InstrumentType::Braids, false}, {"SegaPSG", InstrumentType::SegaPSG, false},
      {"AY classic(AY1)", InstrumentType::AY1, false}, {"AY plus(AY2)", InstrumentType::AY2, false},
      {"AY sample", InstrumentType::AYSample, false}, {"Plaits", InstrumentType::Plaits, false},
      {"PlaitsAlt", InstrumentType::PlaitsAlt, false}, {"SCWF", InstrumentType::SCWF, false},
      {"BYOWTBL", InstrumentType::BYOWTBL, false}, {"AChChid", InstrumentType::AChChid, false},
      {"DrumSynth", InstrumentType::DrumSynth, true}, {"MME", InstrumentType::MME, false},
      {"Sintered", InstrumentType::Sintered, true}, {"OPLL", InstrumentType::OPLL, false},
      {"VRC7", InstrumentType::VRC7, false}, {"OPL2", InstrumentType::OPL2, false},
      {"OPL3", InstrumentType::OPL3, false}, {"GBPulse", InstrumentType::GBPulse, false},
      {"GBNoise", InstrumentType::GBNoise, false}, {"GenesisFM", InstrumentType::GenesisFM, false},
      {"ArcadeFM", InstrumentType::ArcadeFM, false}, {"SID", InstrumentType::SID, false},
  };
  for (auto& e : eng) {
    v.push_back({kEngine, e.n, (int)e.t, 1, 0, e.dense});
    v.push_back({kEngine, e.n, (int)e.t, 4, 0, e.dense});
  }
  for (int m = insertCompressor; m < insertModuleCount; ++m) {
    const char* n = insertDescriptor(m).name;
    v.push_back({kInsert, n, m, 0, 0, false});
    v.push_back({kInsert, n, m, 0, 1, false});
    v.push_back({kInsert, n, m, 0, 2, false});
  }
  const char* mn[] = {"psg limiter off", "psg limiter on", "psg + Clouds reverb send", "psg + Beads reverb send", "psg + delay send"};
  for (int i = 0; i < 5; ++i) v.push_back({kMaster, mn[i], (int)InstrumentType::SegaPSG, 1, i, false});
  v.push_back({kMaster, "idle (no voices) limiter on", (int)InstrumentType::none, 1, 1, false});
  return v;
}
const char* variantName(const Item& it) {
  if (it.kind == kInsert) return it.variant == 0 ? "full" : it.variant == 1 ? "lite" : "silent-bypass";
  if (it.kind == kMaster) return "-";
  return it.voices == 1 ? "1v" : "4v";
}
const char* kindName(const Item& it) { return it.kind == kEngine ? "engine" : it.kind == kInsert ? "insert" : "master"; }

// ---- progress screen (/dev/fb0, 240x240 RGB565): a bar and one square per item ----
struct Screen {
  uint8_t* mem = nullptr;
  size_t len = 0;
  int stride = 0, fd = -1;
  void open_() {
#ifdef __linux__
    const char* path = getenv("RGNANO_FB_DEV");
    if (!path || !path[0]) path = "/dev/fb0";
    fd = open(path, O_RDWR);
    if (fd < 0) return;
    fb_var_screeninfo var;
    fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix) || var.bits_per_pixel != 16 ||
        var.xres != 240 || var.yres != 240) { close(fd); fd = -1; return; }
    stride = (int)fix.line_length;
    len = fix.smem_len;
    void* m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { close(fd); fd = -1; return; }
    mem = (uint8_t*)m;
#endif
  }
  void rect(int x, int y, int w, int h, uint16_t c) {
    if (!mem) return;
    for (int j = y; j < y + h && j < 240; ++j)
      for (int i = x; i < x + w && i < 240; ++i) *(uint16_t*)(mem + j * stride + i * 2) = c;
  }
  static uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
  // pass: 0 first, 1 second, 2 done. status per item: 0 todo, 1 ok, 2 failed.
  void draw(int pass, int passes, int index, int total, const std::vector<int>& status, double elapsed, double eta) {
    if (!mem) return;
    rect(0, 0, 240, 240, rgb(10, 10, 20));
    rect(0, 0, 240, 14, pass == 2 ? rgb(0, 160, 60) : pass == 0 ? rgb(40, 90, 200) : rgb(210, 130, 30));
    for (int p = 0; p < passes; ++p) rect(8 + p * 118, 24, 108, 8, p < pass || pass == 2 ? rgb(0, 200, 90) : rgb(60, 60, 80));
    const int totalSteps = total * passes;
    const int done = std::min(totalSteps, pass * total + index);
    rect(8, 44, 224, 16, rgb(40, 40, 60));
    rect(8, 44, totalSteps ? 224 * done / totalSteps : 0, 16, pass == 2 ? rgb(0, 200, 90) : rgb(230, 230, 240));
    const int cols = 20, cell = 11;
    for (int i = 0; i < total; ++i) {
      uint16_t c = status[i] == 1 ? rgb(0, 190, 80) : status[i] == 2 ? rgb(220, 40, 40) : (i == index && pass < 2) ? rgb(250, 230, 60) : rgb(50, 50, 70);
      rect(8 + (i % cols) * cell, 76 + (i / cols) * cell, cell - 2, cell - 2, c);
    }
    // One 4 px tick per minute: elapsed (grey) and estimated remaining (brown).
    rect(8, 200, std::min(224, (int)(elapsed / 60) * 4), 6, rgb(150, 150, 170));
    if (eta > 0) rect(8, 210, std::min(224, (int)(eta / 60) * 4), 6, rgb(110, 90, 40));
  }
};

// ---- log ----
FILE* gLog = nullptr;
void logf_(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logf_(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  if (gLog) { vfprintf(gLog, fmt, ap); fflush(gLog); fsync(fileno(gLog)); }
  va_end(ap);
  va_start(ap, fmt);
  vfprintf(stdout, fmt, ap);
  fflush(stdout);
  va_end(ap);
}

std::string dataDir() {
  const char* env = getenv("RGNANO_DATA_DIR");
  std::string d = env && env[0] ? env : "/mnt/ChooChooTracker";
  mkdir(d.c_str(), 0755);
  std::string probe = d + "/.write-probe";
  FILE* f = fopen(probe.c_str(), "w");
  if (f) { fclose(f); unlink(probe.c_str()); return d; }
  d = "/tmp/ChooChooTracker";
  mkdir(d.c_str(), 0755);
  return d;
}

std::string firstLineWith(const char* file, const char* key) {
  FILE* f = fopen(file, "r");
  if (!f) return "";
  char line[512];
  std::string out;
  while (fgets(line, sizeof line, f))
    if (strstr(line, key)) {
      const char* c = strchr(line, ':');
      out = c ? c + 1 : line;
      while (!out.empty() && (out[0] == ' ' || out[0] == '\t')) out.erase(0, 1);
      while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
      break;
    }
  fclose(f);
  return out;
}
std::string readSmall(const char* file) {
  FILE* f = fopen(file, "r");
  if (!f) return "?";
  char line[128] = {};
  if (!fgets(line, sizeof line, f)) line[0] = 0;
  fclose(f);
  std::string s = line;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s.empty() ? "?" : s;
}

bool setFifo() {
  sched_param sp;
  memset(&sp, 0, sizeof sp);
  sp.sched_priority = 70;  // same as rgnano_platform.cpp kFifoPriority
  return sched_setscheduler(0, SCHED_FIFO, &sp) == 0;
}

// Runs one item in a child; returns false if the child died or timed out.
bool runInChild(const Item& it, bool fifo, Result* res, std::string* why) {
  int fds[2];
  if (pipe(fds)) { *why = "pipe"; return false; }
  fflush(nullptr);
  pid_t pid = fork();
  if (pid < 0) { *why = "fork"; return false; }
  if (pid == 0) {
    close(fds[0]);
    bool pinned = true;
    if (fifo) pinned = setFifo();
    Result r;
    bool ok = runItem(it, &r);
    if (fifo && !pinned) r.ok = 2;
    ssize_t w = write(fds[1], &r, sizeof r);
    (void)w;
    _exit(ok ? 0 : 3);
  }
  close(fds[1]);
  const double limit = 12.0 * (gSecs + 4) + 30;
  double start = nowSec();
  int status = 0;
  bool exited = false;
  while (nowSec() - start < limit) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) { exited = true; break; }
    usleep(100 * 1000);
  }
  if (!exited) { kill(pid, SIGKILL); waitpid(pid, &status, 0); *why = "timeout"; close(fds[0]); return false; }
  ssize_t n = read(fds[0], res, sizeof *res);
  close(fds[0]);
  if (n != (ssize_t)sizeof *res) {
    char b[64];
    if (WIFSIGNALED(status)) snprintf(b, sizeof b, "killed signal %d (OOM?)", WTERMSIG(status));
    else snprintf(b, sizeof b, "exit %d", WEXITSTATUS(status));
    *why = b;
    return false;
  }
  if (res->ok == 2) *why = "SCHED_FIFO refused";
  return res->ok != 0;
}
}  // namespace

int main(int argc, char** argv) {
  std::string logPath, estPath, hostEst, only, passesArg = "fifo,normal";
  bool useFb = true;
  for (int i = 1; i < argc; ++i) {
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (!strcmp(argv[i], "--secs")) gSecs = atoi(next());
    else if (!strcmp(argv[i], "--warm")) gWarm = atoi(next());
    else if (!strcmp(argv[i], "--max-secs")) gMaxSecs = atoi(next());
    else if (!strcmp(argv[i], "--log")) logPath = next();
    else if (!strcmp(argv[i], "--est")) estPath = next();
    else if (!strcmp(argv[i], "--assets")) gAssets = next();
    else if (!strcmp(argv[i], "--passes")) passesArg = next();
    else if (!strcmp(argv[i], "--only")) only = next();
    else if (!strcmp(argv[i], "--host-est")) hostEst = next();
    else if (!strcmp(argv[i], "--no-fb")) useFb = false;
    else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
  }
  if (logPath.empty()) logPath = dataDir() + "/bench.txt";
  gLog = fopen(logPath.c_str(), "w");
  if (!hostEst.empty()) passesArg = "normal";

  std::vector<Item> items;
  for (auto& it : buildItems())
    if (only.empty() || it.key().find(only) != std::string::npos) items.push_back(it);
  std::vector<bool> passFifo;
  if (passesArg.find("fifo") != std::string::npos) passFifo.push_back(true);
  if (passesArg.find("normal") != std::string::npos) passFifo.push_back(false);

  Screen screen;
  if (useFb && hostEst.empty()) screen.open_();

  logf_("# rgnano-bench2 (ChooChooTracker FX/engine cost battery), %d Hz, %d-frame blocks, %d s measured + %d s warmup per item\n",
        kRate, kBlock, gSecs, gWarm);
  std::string model = firstLineWith("/proc/cpuinfo", "model name");
  if (model.empty()) model = firstLineWith("/proc/cpuinfo", "Processor");
  std::string hw = firstLineWith("/proc/cpuinfo", "Hardware");
  const char* fp = "/sys/devices/system/cpu/cpu0/cpufreq/";
  std::string gov = readSmall((std::string(fp) + "scaling_governor").c_str());
  long f0 = readFreqKhz();
  long fmin = f0, fmax = f0;
  logf_("# cpu: model=\"%s\" hardware=\"%s\" governor=%s start_freq_khz=%ld\n", model.c_str(), hw.c_str(), gov.c_str(), f0);
  logf_("# raw rows (as measured), tab separated:\n");
  logf_("mode\tkind\titem\tvariant\tmean_pct_rt\tmax_pct_rt\tp99_pct_rt\tblocks\tskip_pct\tfreq_khz\tstatus\n");

  struct Row { Result r[2]; std::string why[2]; };
  std::vector<Row> rows(items.size());
  const double t0 = nowSec();
  std::vector<int> status(items.size(), 0);
  for (size_t pi = 0; pi < passFifo.size(); ++pi) {
    const bool fifo = passFifo[pi];
    std::fill(status.begin(), status.end(), 0);
    for (size_t i = 0; i < items.size(); ++i) {
      double elapsed = nowSec() - t0;
      size_t doneItems = pi * items.size() + i;
      double perItem = doneItems ? elapsed / doneItems : 8.0;
      double eta = perItem * (passFifo.size() * items.size() - doneItems);
      screen.draw((int)pi, (int)passFifo.size(), (int)i, (int)items.size(), status, elapsed, eta);
      long fb = readFreqKhz();
      if (fb > 0) { fmin = fmin < 0 ? fb : std::min(fmin, fb); fmax = std::max(fmax, fb); }
      Result r;
      std::string why;
      bool ok = false;
      if (elapsed > gMaxSecs) why = "skipped (overall time limit)";
      else ok = runInChild(items[i], fifo, &r, &why);
      const int slot = fifo ? 0 : 1;
      if (!ok) memset(&r, 0, sizeof r);
      rows[i].r[slot] = r;
      rows[i].why[slot] = why;
      status[i] = ok ? 1 : 2;
      if (r.freqKhz > 0) { fmin = fmin < 0 ? r.freqKhz : std::min(fmin, r.freqKhz); fmax = std::max(fmax, r.freqKhz); }
      logf_("%s\t%s\t%s\t%s\t%.2f\t%.2f\t%.2f\t%d\t%.1f\t%ld\t%s\n", fifo ? "fifo70" : "normal", kindName(items[i]),
            items[i].name.c_str(), variantName(items[i]), r.mean, r.max, r.p99, r.blocks, r.skipPct, r.freqKhz,
            why.empty() ? "ok" : why.c_str());
    }
  }
  screen.draw(2, (int)passFifo.size(), (int)items.size(), (int)items.size(), status, nowSec() - t0, 0);

  if (!hostEst.empty()) {
    FILE* f = fopen(hostEst.c_str(), "w");
    if (f) {
      fprintf(f, "# key\tx86_mean_pct_rt\tA7_estimate_pct_rt (x86 x %.0f)\n", kA7Factor);
      for (size_t i = 0; i < items.size(); ++i)
        fprintf(f, "%s\t%.4f\t%.4f\n", items[i].key().c_str(), rows[i].r[1].mean, rows[i].r[1].mean * kA7Factor);
      fclose(f);
    }
    logf_("# host estimate table written to %s\n", hostEst.c_str());
    if (gLog) { fflush(gLog); fsync(fileno(gLog)); fclose(gLog); }
    return 0;
  }

  std::map<std::string, double> est;
  if (!estPath.empty()) {
    FILE* f = fopen(estPath.c_str(), "r");
    if (f) {
      char line[512];
      while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char* a = strtok(line, "\t");
        char* b = strtok(nullptr, "\t");
        char* c = strtok(nullptr, "\t\n");
        if (a && b && c) est[a] = atof(c);
      }
      fclose(f);
    }
  }

  const long fEnd = readFreqKhz();
  if (fEnd > 0) { fmin = std::min(fmin, fEnd); fmax = std::max(fmax, fEnd); }
  const bool haveFifo = !passFifo.empty() && passFifo[0];
  const bool haveNorm = std::find(passFifo.begin(), passFifo.end(), false) != passFifo.end();
  const int sF = 0, sN = 1;
  logf_("\n# summary: cpu=\"%s\" governor=%s freq_khz start=%ld end=%ld min=%ld max=%ld changed_during_run=%s total_secs=%.0f\n",
        model.c_str(), gov.c_str(), f0, fEnd, fmin, fmax, fmin != fmax ? "YES" : "no", nowSec() - t0);
  logf_("# fifo = SCHED_FIFO prio 70 (as the audio thread); normal = SCHED_OTHER. pct = render time / audio time x100; max = worst 512-frame block.\n");
  logf_("# est_a7 = x86 run of this same battery x%.0f (the factor fx_profile used); ratio = real mean / est_a7 (multiply fx_profile numbers by it).\n",
        kA7Factor);
  logf_("section\titem\tvariant\tfifo_mean\tfifo_max\tnormal_mean\tnormal_max\tnet_per_voice\test_a7_mean\tratio_real_over_est\n");
  double baseF = 0, baseN = 0;
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i].name == "BASELINE-idle") { baseF = rows[i].r[sF].mean; baseN = rows[i].r[sN].mean; }
  for (size_t i = 0; i < items.size(); ++i) {
    const Item& it = items[i];
    const Result& rf = rows[i].r[sF];
    const Result& rn = rows[i].r[sN];
    char net[32] = "-";
    if (it.kind == kEngine && it.name != "BASELINE-idle")
      snprintf(net, sizeof net, "%.2f", haveFifo ? (rf.mean - baseF) / it.voices : (rn.mean - baseN) / it.voices);
    char e[32] = "-", ratio[32] = "-";
    auto found = est.find(it.key());
    if (found != est.end()) {
      snprintf(e, sizeof e, "%.2f", found->second);
      const double real = haveFifo ? rf.mean : rn.mean;
      if (found->second > 0.005) snprintf(ratio, sizeof ratio, "%.2f", real / found->second);
    }
    char fm[16] = "-", fx[16] = "-", nm[16] = "-", nx[16] = "-";
    if (haveFifo) { snprintf(fm, sizeof fm, "%.2f", rf.mean); snprintf(fx, sizeof fx, "%.2f", rf.max); }
    if (haveNorm) { snprintf(nm, sizeof nm, "%.2f", rn.mean); snprintf(nx, sizeof nx, "%.2f", rn.max); }
    logf_("%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", kindName(it), it.name.c_str(), variantName(it), fm, fx, nm, nx, net, e, ratio);
  }
  // Derived master rows: each stage against the limiter-off PSG row.
  size_t base = items.size();
  for (size_t i = 0; i < items.size(); ++i)
    if (items[i].kind == kMaster && items[i].variant == 0 && items[i].type == (int)InstrumentType::SegaPSG) base = i;
  if (base < items.size()) {
    const int sl = haveFifo ? sF : sN;
    logf_("\n# master stages, net of the psg limiter-off row (pct_rt, %s mean): ", haveFifo ? "fifo" : "normal");
    for (size_t i = 0; i < items.size(); ++i)
      if (items[i].kind == kMaster && items[i].variant > 0 && items[i].type == (int)InstrumentType::SegaPSG)
        logf_("[%s %+.2f] ", items[i].name.c_str(), rows[i].r[sl].mean - rows[base].r[sl].mean);
    logf_("\n");
  }
  logf_("\n# FX ratio real_A7/estimate (mean over est_a7), one line per insert; rescale the fx_profile table by these:\n");
  logf_("fx\tfull\tlite\tsilent_bypass\n");
  for (int m = insertCompressor; m < insertModuleCount; ++m) {
    char cell[3][24];
    for (int v = 0; v < 3; ++v) {
      snprintf(cell[v], sizeof cell[v], "-");
      for (size_t i = 0; i < items.size(); ++i)
        if (items[i].kind == kInsert && items[i].type == m && items[i].variant == v) {
          auto found = est.find(items[i].key());
          const double real = haveFifo ? rows[i].r[sF].mean : rows[i].r[sN].mean;
          if (found != est.end() && found->second > 0.005) snprintf(cell[v], sizeof cell[v], "%.2f", real / found->second);
        }
    }
    logf_("%s\t%s\t%s\t%s\n", insertDescriptor(m).name, cell[0], cell[1], cell[2]);
  }
  logf_("# done\n");
  if (gLog) { fflush(gLog); fsync(fileno(gLog)); fclose(gLog); }
  return 0;
}
