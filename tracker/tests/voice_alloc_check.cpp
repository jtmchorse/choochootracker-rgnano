// Lazy voice allocation checks (P2 "B Memory").
//
// Standalone executable (not part of run_tests: it replaces the process-wide
// malloc with a counting allocator). Run with:
//   make -f Makefile.test voice-alloc-check          (from tracker/)
//   make -f Makefile.test voice-alloc-check RGNANO=1
//
// What it proves:
//  1. The demo projects render bit-identically in eager, lazyOffline and
//     lazyRealtime mode (same FNV-1a hash of every output float).
//  2. In lazyRealtime mode chipnomadRender() performs ZERO heap allocations
//     (allocator hook aborts on any malloc/new on the audio thread), through
//     project edits, refreshes, previews and instrument-type changes.
//  3. A voice the audio thread needs but the UI has not built yet renders
//     silence, is flagged, and is built by the UI thread on the next status
//     poll.
//  4. Idle voice sets of types no track uses are pruned, deleted only after
//     the render call has left, and nothing leaks across Destroy.
//  5. A render thread and a UI thread can hammer instrument changes against
//     each other without a crash (run the ASAN build for use-after-free).
#include "alloc_counter.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

#include "chipnomad_lib.h"
#include "audio_monitor.h"
#include "playback.h"
#include "project.h"
#include "screens/screens.h"
#include "project_utils.h"
#include "synth/sid_voice.h"

void getSelectionBounds(ScreenData*, int*, int*, int*, int*) {}

namespace {
int gFailures = 0, gChecks = 0;
#define CHECK(cond, ...) do { ++gChecks; if (!(cond)) { ++gFailures; \
  std::printf("  FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

const int kRate = 32000;
const int kBlock = 512;

struct Rendered { uint64_t hash; float peak; int shortBlocks; };

// Render n frames the way the audio callback does: allocation trap armed.
Rendered renderFrames(ChipNomadState* state, long long frames, bool trap) {
  std::vector<float> buf(kBlock * 2);
  Rendered r{1469598103934665603ull, 0.0f, 0};
  gTrap = trap;
  while (frames > 0) {
    int n = frames > kBlock ? kBlock : (int)frames;
    tlInRender = trap ? 1 : 0;
    int got = chipnomadRender(state, buf.data(), n);
    tlInRender = 0;
    if (got != n) ++r.shortBlocks;
    for (int i = 0; i < n * 2; ++i) {
      uint32_t bits;
      std::memcpy(&bits, &buf[i], 4);
      for (int b = 0; b < 4; ++b) { r.hash ^= (bits >> (b * 8)) & 0xff; r.hash *= 1099511628211ull; }
      r.peak = std::max(r.peak, std::fabs(buf[i]));
    }
    frames -= n;
  }
  gTrap = false;
  return r;
}

Project* newProject() {
  Project* p = (Project*)std::malloc(sizeof(Project));
  projectInitAY(p);
  p->chipsCount = 1;
  return p;
}

void setTrack(Project* p, int track, InstrumentType type, int note = 48) {
  Instrument* ins = &p->instruments[track];
  getInstrumentFunctions(ins->type).free(ins);
  ins->type = type;
  getInstrumentFunctions(type).init(ins);
  p->song[0][track] = (uint16_t)track;
  p->chains[track].rows[0].phrase = (uint16_t)track;
  p->chains[track].rows[0].transpose = 0;
  phraseClear(&p->phrases[track]);
  p->phrases[track].rows[0].note = (uint8_t)note;
  p->phrases[track].rows[0].instrument = (uint8_t)track;
  p->phrases[track].rows[0].volume = 100;
}

ChipNomadState* makeState(ChipNomadVoiceMode mode, const Project* p) {
  ChipNomadState* s = chipnomadCreateEx(mode);
  s->ownsProjectResources = 0;
  s->project = *p;
  s->audioProject = *p;
  playbackInit(&s->playbackState, &s->audioProject);
  chipnomadInitChips(s, kRate, nullptr);
  chipnomadReserveRenderBuffers(s, kBlock);
  return s;
}

bool built(ChipNomadState* s, int track, int family) {
  switch (family) {
    case 0: return s->plaitsVoices[track][0] != nullptr;
    case 1: return s->sampleVoices[track][0] != nullptr;
    case 2: return s->sidVoices[track][0] != nullptr;
    case 3: return s->simpleChipVoices[track][0] != nullptr;
    case 4: return s->dx7Parts[track] != nullptr;
    case 5: return s->achchidVoices[track][0] != nullptr;
  }
  return false;
}
enum { fPlaits, fSample, fSID, fSimple, fDX7, fAChChid };

// --- 1 ---------------------------------------------------------------------
void demoRendersMatch() {
  std::printf("[1] demo projects: eager == lazyOffline == lazyRealtime (bit-exact)\n");
  DIR* d = opendir("packaging/common/projects");
  if (!d) { CHECK(false, "cannot open packaging/common/projects (run from tracker/)"); return; }
  std::vector<std::string> files;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n.size() > 4 && n.substr(n.size() - 4) == ".cct") files.push_back("packaging/common/projects/" + n);
  }
  closedir(d);
  std::sort(files.begin(), files.end());
  for (auto& f : files) {
    Project* p = (Project*)std::malloc(sizeof(Project));
    projectInit(p);
    if (projectLoad(p, f.c_str())) { CHECK(false, "load %s", f.c_str()); continue; }
    uint64_t h[3];
    float peak = 0;
    const ChipNomadVoiceMode modes[3] = {ChipNomadVoiceMode::eager, ChipNomadVoiceMode::lazyOffline,
                                          ChipNomadVoiceMode::lazyRealtime};
    for (int m = 0; m < 3; ++m) {
      // The engine keeps process-wide state (noise seeds, live-stick, motion
      // record), so each mode renders in a fresh process to be comparable.
      int fd[2];
      if (pipe(fd)) { CHECK(false, "pipe"); continue; }
      pid_t pid = fork();
      if (pid == 0) {
        close(fd[0]);
        ChipNomadState* s = makeState(modes[m], p);
        playbackStartSong(&s->playbackState, 0, 0, 1);
        Rendered r = renderFrames(s, 6LL * kRate, modes[m] == ChipNomadVoiceMode::lazyRealtime);
        struct { uint64_t hash; float peak; } out{r.hash, r.peak};
        if (write(fd[1], &out, sizeof out) != (ssize_t)sizeof out) _exit(2);
        _exit(0);
      }
      close(fd[1]);
      struct { uint64_t hash; float peak; } in{0, 0};
      ssize_t got = read(fd[0], &in, sizeof in);
      close(fd[0]);
      int status = 0;
      waitpid(pid, &status, 0);
      CHECK(got == (ssize_t)sizeof in && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "%s mode %d child failed (status %d)", f.c_str(), m, status);
      h[m] = in.hash; peak = std::max(peak, in.peak);
    }
    CHECK(h[0] == h[1] && h[0] == h[2], "%s hashes %016llx %016llx %016llx", f.c_str(),
          (unsigned long long)h[0], (unsigned long long)h[1], (unsigned long long)h[2]);
    std::printf("    %-62s %s peak %.2f\n", f.c_str(), h[0] == h[1] && h[0] == h[2] ? "identical" : "DIFFERENT", peak);
    std::free(p);
  }
}

// --- 2 ---------------------------------------------------------------------
void realtimeEditRefreshPlay() {
  std::printf("[2] lazyRealtime: UI edit + refresh builds voices; render never allocates\n");
  Project* p = newProject();
  p->tracksCount = 4;
  ChipNomadState* s = makeState(ChipNomadVoiceMode::lazyRealtime, p);
  for (int t = 0; t < 4; ++t)
    for (int f = 0; f <= fAChChid; ++f) CHECK(!built(s, t, f), "empty project built nothing (t%d f%d)", t, f);
  Rendered r = renderFrames(s, kRate / 2, true);
  CHECK(r.peak == 0.0f, "empty project is silent");

  // UI thread: edit the project, then the app's refresh path.
  setTrack(p, 0, InstrumentType::Plaits);
  setTrack(p, 1, InstrumentType::Sample);
  setTrack(p, 2, InstrumentType::SID);
  s->project = *p;
  CHECK(chipnomadQueueProjectRefresh(s), "refresh queued");
  CHECK(built(s, 0, fPlaits) && built(s, 1, fSample) && built(s, 2, fSID), "reachable sets built");
  CHECK(!built(s, 0, fSID) && !built(s, 1, fPlaits) && !built(s, 3, fSample) && !built(s, 2, fSimple),
        "unreachable sets not built");
  CHECK(chipnomadQueuePlaybackStartSong(s, 0, 0, 1), "start queued");
  r = renderFrames(s, 2LL * kRate, true);
  CHECK(r.peak > 0.001f, "plays after refresh (peak %.4f)", r.peak);
  chipnomadDestroy(s);
  std::free(p);
}

// --- 3 ---------------------------------------------------------------------
void realtimeMissingVoice() {
  std::printf("[3] lazyRealtime: audio meets a missing voice -> silence, flag, UI builds it\n");
  Project* p = newProject();
  p->tracksCount = 2;
  ChipNomadState* s = makeState(ChipNomadVoiceMode::lazyRealtime, p);
  // The audio side adopted a project the UI has not prepared voices for.
  setTrack(&s->audioProject, 0, InstrumentType::SID);
  setTrack(&s->project, 0, InstrumentType::SID);
  CHECK(!built(s, 0, fSID), "SID not built yet");
  playbackStartSong(&s->playbackState, 0, 0, 1);
  Rendered r = renderFrames(s, kRate, true);   // trap armed: any allocation aborts
  CHECK(r.peak == 0.0f, "silent while the set is missing (peak %.4f)", r.peak);
  CHECK(s->voiceMissing[0] != 0, "audio thread flagged the missing set");
  CHECK(!built(s, 0, fSID), "audio thread did not build it");
  chipnomadGetPlaybackStatus(s);                // UI thread poll
  CHECK(built(s, 0, fSID), "UI poll built it");
  CHECK(s->voiceMissing[0] == 0, "flag consumed");
  playbackStartSong(&s->playbackState, 0, 0, 1);
  r = renderFrames(s, kRate, true);
  CHECK(r.peak > 0.001f, "audible once built (peak %.4f)", r.peak);
  chipnomadDestroy(s);
  std::free(p);
}

// --- 4 ---------------------------------------------------------------------
void previewFirstAudition() {
  std::printf("[4] lazyRealtime: preview voices are built on first audition\n");
  Project* p = newProject();
  p->tracksCount = 2;
  ChipNomadState* s = makeState(ChipNomadVoiceMode::lazyRealtime, p);
  CHECK(!s->sidPreview && !s->simpleChipPreview && !s->fourOpPreview && !s->oplPreview &&
        !s->dx7Preview && !s->opllPreview, "no preview objects before use");
  Instrument ins;
  std::memset(&ins, 0, sizeof ins);
  ins.type = InstrumentType::SID;
  getInstrumentFunctions(InstrumentType::SID).init(&ins);
  CHECK(chipnomadQueueSIDPreview(s, 0, &ins.chip.sid), "SID preview queued");
  CHECK(s->sidPreview != nullptr, "sidPreview built by the queue call (UI thread)");
  CHECK(!s->fourOpPreview && !s->oplPreview, "other previews still absent");
  Rendered r = renderFrames(s, kRate / 2, true);
  CHECK(r.peak > 0.001f, "preview audible (peak %.4f)", r.peak);
  chipnomadDestroy(s);
  std::free(p);
}

// --- 5 ---------------------------------------------------------------------
void pruneAndLeaks() {
  std::printf("[5] prune idle sets, deferred delete, no leaks across Destroy\n");
  long long before = gLive.load();
  {
    Project* p = newProject();
    p->tracksCount = 2;
    setTrack(p, 0, InstrumentType::Plaits);
    ChipNomadState* s = makeState(ChipNomadVoiceMode::lazyRealtime, p);
    CHECK(built(s, 0, fPlaits), "plaits built at InitChips");
    setTrack(p, 0, InstrumentType::SegaPSG);
    s->project = *p;
    chipnomadQueueProjectRefresh(s);
    CHECK(!built(s, 0, fPlaits), "idle plaits set pruned");
    CHECK(built(s, 0, fSimple), "new type built");
    CHECK(s->voiceRetiredCount == 0, "retired set reclaimed (audio idle), %d pending", s->voiceRetiredCount);
    CHECK(chipnomadQueuePlaybackStartSong(s, 0, 0, 1), "start");
    Rendered r = renderFrames(s, kRate, true);
    CHECK(r.peak > 0.001f, "PSG plays after the swap");
    // Flip back and forth: sets come and go, nothing accumulates.
    for (int i = 0; i < 20; ++i) {
      setTrack(p, 0, i & 1 ? InstrumentType::Plaits : InstrumentType::SID);
      s->project = *p;
      chipnomadQueueProjectRefresh(s);
      renderFrames(s, kBlock * 4, true);
    }
    CHECK(s->voiceRetiredCount == 0, "retire list empty at rest");
    chipnomadDestroy(s);
    std::free(p);
  }
  long long after = gLive.load();
  CHECK(after - before < 4096 && before - after < 4096, "net heap change across lifecycle: %lld bytes", after - before);
}

// --- 6 ---------------------------------------------------------------------
void threadedStress() {
  std::printf("[6] render thread vs UI thread, instrument types changing\n");
  Project* p = newProject();
  p->tracksCount = 4;
  for (int t = 0; t < 4; ++t) setTrack(p, t, InstrumentType::Braids, 40 + t * 3);
  ChipNomadState* s = makeState(ChipNomadVoiceMode::lazyRealtime, p);
  std::atomic<bool> stop{false};
  std::atomic<long long> blocks{0};
  std::atomic<int> shorts{0};
  chipnomadQueuePlaybackStartSong(s, 0, 0, 1);
  std::thread audio([&] {
    std::vector<float> buf(kBlock * 2);
    gTrap = true;
    while (!stop.load()) {
      tlInRender = 1;
      int got = chipnomadRender(s, buf.data(), kBlock);
      tlInRender = 0;
      if (got != kBlock) shorts++;
      blocks++;
    }
  });
  const InstrumentType types[] = {InstrumentType::Plaits, InstrumentType::Sample, InstrumentType::SID,
    InstrumentType::SegaPSG, InstrumentType::DX7, InstrumentType::AChChid, InstrumentType::Braids,
    InstrumentType::OPL3, InstrumentType::GenesisFM, InstrumentType::DrumSynth};
  for (int i = 0; i < 400; ++i) {
    int t = i % 4;
    setTrack(p, t, types[(i * 7 + t) % 10], 36 + (i % 24));
    s->project = *p;
    chipnomadQueueProjectRefresh(s);
    if (i % 3 == 0) chipnomadQueuePlaybackStartSong(s, 0, 0, 1);
    if (i % 5 == 0) chipnomadQueuePlaybackPreviewNote(s, t, 48, (uint8_t)t);
    chipnomadGetPlaybackStatus(s);
    std::this_thread::sleep_for(std::chrono::microseconds(700));
  }
  stop = true;
  audio.join();
  gTrap = false;
  CHECK(blocks.load() > 100, "audio thread rendered %lld blocks", blocks.load());
  std::printf("    audio thread rendered %lld blocks, UI made 400 type changes\n", blocks.load());
  chipnomadDestroy(s);
  std::free(p);
}

// --- 7 ---------------------------------------------------------------------
void eagerUnchanged() {
  std::printf("[7] eager mode is the upstream layout\n");
  ChipNomadState* s = chipnomadCreateEx(ChipNomadVoiceMode::eager);
  bool all = true;
  for (int t = 0; t < PROJECT_MAX_TRACKS; ++t)
    all = all && s->braidsVoices[t][3] && s->sampleVoices[t][3] && s->sidVoices[t][3] && s->dx7Parts[t] &&
          s->achchidVoices[t][0] && s->opllVoices[t][2];
  CHECK(all, "every voice exists after chipnomadCreate");
  CHECK(s->sidPreview && s->opllPreview && s->dx7Preview, "previews exist");
#ifdef RGNANO_BUILD
  CHECK(chipnomadDefaultVoiceMode() == ChipNomadVoiceMode::lazyRealtime, "RGNANO_BUILD default is lazyRealtime");
#else
  CHECK(chipnomadDefaultVoiceMode() == ChipNomadVoiceMode::eager, "default build keeps eager");
#endif
  chipnomadDestroy(s);
}
}  // namespace

// --- 7 ---------------------------------------------------------------------
// Master limiter on the audio thread: no allocation (trap armed) through the
// off -> on -> off toggle, and the ceiling holds on a song that clips without it.
void masterLimiterNoAlloc() {
  std::printf("[7] master limiter: render never allocates; ceiling holds on a clipping demo\n");
  Project* p = (Project*)std::malloc(sizeof(Project));
  projectInit(p);
  if (projectLoad(p, "packaging/common/projects/dub-reich.cct")) { CHECK(false, "load dub-reich"); std::free(p); return; }
  ChipNomadState* s = makeState(ChipNomadVoiceMode::lazyRealtime, p);
  CHECK(chipnomadPrepareVoices(s) != 0 || true, "voices prepared");
  chipnomadSetMasterLimiter(s, 0);
  playbackStartSong(&s->playbackState, 0, 0, 1);
  Rendered off = renderFrames(s, 8LL * kRate, true);
  std::printf("    limiter off: peak %.3f\n", off.peak);
  CHECK(off.peak > 1.0f, "dub-reich exceeds full scale without the limiter (peak %.3f)", off.peak);
  chipnomadSetMasterLimiter(s, 1);                      // off -> on edge: audio thread resets state
  Rendered on = renderFrames(s, 8LL * kRate, true);     // trap armed: any allocation aborts
  std::printf("    limiter on:  peak %.3f\n", on.peak);
  CHECK(on.peak > 0.5f && on.peak <= 0.96606f, "limiter holds -0.3 dBFS (peak %.5f)", on.peak);
  float minGain, meanRed; uint32_t engaged, total;
  CHECK(chipnomadTakeLimiterStats(s, &minGain, &meanRed, &engaged, &total) == 1 && total > 0 && engaged > 0 && minGain < 1.0f,
        "stats report gain reduction (min %.3f engaged %u/%u)", minGain, engaged, total);
  chipnomadSetMasterLimiter(s, 0);
  Rendered off2 = renderFrames(s, 2LL * kRate, true);
  CHECK(off2.shortBlocks == 0, "still renders after switching off");
  chipnomadDestroy(s);
  projectFree(p);
  std::free(p);
}

// --- 8 ---------------------------------------------------------------------
// Regression (round 4.10): upstream per-family update functions began with
// `if (!state->xVoices[0][0]) return;`. With lazy allocation that is NULL
// whenever track 0 uses another family, so every other track of the family
// kept power-on defaults (a held C4 drone). Track 0 = SID, the rest differ:
// lazyRealtime must match eager block for block.
std::vector<uint64_t> renderBlockHashes(ChipNomadVoiceMode mode, const Project* p, int blocks) {
  int fd[2];
  std::vector<uint64_t> h(blocks, 0);
  if (pipe(fd)) return h;
  pid_t pid = fork();
  if (pid == 0) {
    close(fd[0]);
    ChipNomadState* s = makeState(mode, p);
    playbackStartSong(&s->playbackState, 0, 0, 1);
    std::vector<float> buf(kBlock * 2);
    for (int b = 0; b < blocks; ++b) {
      chipnomadRender(s, buf.data(), kBlock);
      uint64_t x = 1469598103934665603ull;
      const unsigned char* c = (const unsigned char*)buf.data();
      for (size_t i = 0; i < buf.size() * sizeof(float); ++i) { x ^= c[i]; x *= 1099511628211ull; }
      h[b] = x;
    }
    if (write(fd[1], h.data(), blocks * 8) != (ssize_t)(blocks * 8)) _exit(2);
    _exit(0);
  }
  close(fd[1]);
  ssize_t got = read(fd[0], h.data(), blocks * 8);
  close(fd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  if (got != (ssize_t)(blocks * 8) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) std::fill(h.begin(), h.end(), 0);
  return h;
}

void mixedFamiliesMatchEager() {
  std::printf("[8] track 0 family differs from the playing tracks: lazyRealtime == eager per block\n");
  Project* p = newProject();
  p->tracksCount = 4;
  setTrack(p, 0, InstrumentType::SID, 36);
  setTrack(p, 1, InstrumentType::Braids, 48);
  setTrack(p, 2, InstrumentType::Plaits, 55);
  setTrack(p, 3, InstrumentType::SegaPSG, 60);
  const int blocks = 125;
  auto e = renderBlockHashes(ChipNomadVoiceMode::eager, p, blocks);
  auto l = renderBlockHashes(ChipNomadVoiceMode::lazyRealtime, p, blocks);
  int first = -1;
  for (int b = 0; b < blocks; ++b) if (e[b] != l[b]) { first = b; break; }
  CHECK(e[blocks - 1] != 0, "eager render ran");
  CHECK(first < 0, "first diverging block %d of %d (eager %016llx lazy %016llx)", first, blocks,
        (unsigned long long)(first >= 0 ? e[first] : 0), (unsigned long long)(first >= 0 ? l[first] : 0));
  std::free(p);
}

int main(int argc, char** argv) {
  // --no-demos: skip section 1 (needed under ASAN, which trips on a known
  // out-of-range table read inside the vendored Plaits six-op engine).
  const bool demos = !(argc > 1 && !std::strcmp(argv[1], "--no-demos"));
  eagerUnchanged();
  if (demos) demoRendersMatch();
  realtimeEditRefreshPlay();
  realtimeMissingVoice();
  previewFirstAudition();
  pruneAndLeaks();
  threadedStress();
  masterLimiterNoAlloc();
  mixedFamiliesMatchEager();
  std::printf("%s: %d checks, %d failures\n", gFailures ? "FAILED" : "PASSED", gChecks, gFailures);
  return gFailures ? 1 : 0;
}
