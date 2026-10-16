// Heap probe for the voice-allocation work (P2 "B Memory").
//
// Replaces the libc allocator entry points with counting versions so the
// process reports exact live-byte high-water marks, and optionally traps
// any allocation that happens inside chipnomadRender() when the engine is in
// realtime-strict mode (the audio-thread no-allocation proof).
//
//   heap_probe [options] [project.cct]
//     --empty            use an empty project (default if no file given)
//     --worst            8 tracks, a different heavy engine on each
//     --mixed4           4 tracks: Sample, AChChid, Plaits, SegaPSG
//     --mode eager|lazy|rt     voice allocation mode (default: build default)
//     --rate N           engine rate (default 32000)
//     --seconds N        seconds to render (default 4)
//     --trap             abort() on any allocation inside the render call
//     --hash             print FNV-1a of every rendered float (A/B compare)
//     --csv              one machine-readable line
//
// Built by `make -f Makefile.test heap-probe`; add RGNANO=1 for the
// -DRGNANO_BUILD variant (separate object directory).
#include <atomic>
#include <execinfo.h>
#include <signal.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <string>
#include <unistd.h>
#include <vector>

#include "chipnomad_lib.h"
#include "audio_monitor.h"
#include "playback.h"
#include "project.h"
#include "screens/screens.h"
#include "project_utils.h"

void getSelectionBounds(ScreenData*, int*, int*, int*, int*) {}

#include "alloc_counter.h"

// ---------------------------------------------------------------------------

namespace {
long long mb(long long b) { return (b + 524288) / 1048576; }
double mbf(long long b) { return b / 1048576.0; }

long rssKb() {
  FILE* f = fopen("/proc/self/status", "r");
  if (!f) return -1;
  char line[256];
  long v = -1;
  while (fgets(line, sizeof line, f))
    if (!strncmp(line, "VmRSS:", 6)) { v = atol(line + 6); break; }
  fclose(f);
  return v;
}

void buildTrackSong(Project* p, int tracks, const InstrumentType* types) {
  p->chipsCount = 1;
  p->tracksCount = tracks;
  for (int t = 0; t < tracks; ++t) {
    phraseClear(&p->phrases[t]);
    Instrument* ins = &p->instruments[t];
    ins->type = types[t];
    getInstrumentFunctions(types[t]).init(ins);
    p->song[0][t] = (uint16_t)t;
    p->chains[t].rows[0].phrase = (uint16_t)t;
    p->chains[t].rows[0].transpose = 0;
    p->phrases[t].rows[0].note = (uint8_t)(36 + t * 3);
    p->phrases[t].rows[0].instrument = (uint8_t)t;
    p->phrases[t].rows[0].volume = 100;
  }
}
}  // namespace

static void onSegv(int) {
  void* frames[32];
  int n = backtrace(frames, 32);
  backtrace_symbols_fd(frames, n, 2);
  _exit(139);
}

int main(int argc, char** argv) {
  signal(SIGSEGV, onSegv);
  const char* path = nullptr;
  std::string project = "empty";
  std::string mode;
  int rate = 32000, seconds = 4;
  bool hash = false, csv = false;
  std::string typesArg;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--empty") project = "empty";
    else if (a == "--worst") project = "worst";
    else if (a == "--mixed4") project = "mixed4";
    else if (a == "--types" && i + 1 < argc) { project = "types"; typesArg = argv[++i]; }
    else if (a == "--mode" && i + 1 < argc) mode = argv[++i];
    else if (a == "--rate" && i + 1 < argc) rate = atoi(argv[++i]);
    else if (a == "--seconds" && i + 1 < argc) seconds = atoi(argv[++i]);
    else if (a == "--trap") gTrap = true;
    else if (a == "--hash") hash = true;
    else if (a == "--csv") csv = true;
    else { path = argv[i]; project = "file"; }
  }

  long long startLive = gLive.load();
  Project* loaded = (Project*)malloc(sizeof(Project));
  projectInitAY(loaded);  // sets tickRate, pitch table, chip setup like a New Project
  if (project == "file") {
    if (projectLoad(loaded, path)) {
      fprintf(stderr, "Cannot load %s: %s\n", path, projectFileError);
      return 1;
    }
  } else if (project == "worst") {
    const InstrumentType t[8] = {InstrumentType::Sample, InstrumentType::AChChid,
      InstrumentType::SegaPSG, InstrumentType::Plaits, InstrumentType::PlaitsAlt,
      InstrumentType::OPL3, InstrumentType::GenesisFM, InstrumentType::SID};
    buildTrackSong(loaded, 8, t);
  } else if (project == "types") {  // numeric InstrumentType ids, comma separated
    InstrumentType t[8];
    int n = 0;
    for (size_t pos = 0; pos < typesArg.size() && n < 8;) {
      t[n++] = (InstrumentType)atoi(typesArg.c_str() + pos);
      size_t c = typesArg.find(',', pos);
      if (c == std::string::npos) break;
      pos = c + 1;
    }
    buildTrackSong(loaded, n, t);
  } else if (project == "mixed4") {
    const InstrumentType t[4] = {InstrumentType::Sample, InstrumentType::AChChid,
      InstrumentType::Plaits, InstrumentType::SegaPSG};
    buildTrackSong(loaded, 4, t);
  }

  long long projectBytes = gLive.load() - startLive;  // .cct payload incl. sample data
  long long baseLive = startLive;
  gPeak.store(gLive.load());

#ifdef CHIPNOMAD_HAS_VOICE_MODE
  ChipNomadVoiceMode vm = chipnomadDefaultVoiceMode();
  if (mode == "eager") vm = ChipNomadVoiceMode::eager;
  else if (mode == "lazy") vm = ChipNomadVoiceMode::lazyOffline;
  else if (mode == "rt") vm = ChipNomadVoiceMode::lazyRealtime;
  ChipNomadState* state = chipnomadCreateEx(vm);
  const char* modeName = vm == ChipNomadVoiceMode::eager ? "eager"
    : vm == ChipNomadVoiceMode::lazyOffline ? "lazy" : "rt";
#else
  ChipNomadState* state = chipnomadCreate();
  const char* modeName = "upstream";
#endif
  if (!state) return 1;
  long long afterCreate = gLive.load();
  state->ownsProjectResources = 0;
  state->project = *loaded;
  state->audioProject = *loaded;
  playbackInit(&state->playbackState, &state->audioProject);
  chipnomadInitChips(state, rate, nullptr);
  chipnomadReserveRenderBuffers(state, 512);
  long long afterInit = gLive.load();
  playbackStartSong(&state->playbackState, 0, 0, 1);

  std::vector<float> buf(512 * 2);
  uint64_t h = 1469598103934665603ull;
  long long frames = (long long)seconds * rate;
  bool trapAudio = gTrap;
  int shortRenders = 0;
  while (frames > 0) {
    int n = frames > 512 ? 512 : (int)frames;
    tlInRender = 1;
    int got = chipnomadRender(state, buf.data(), n);
    tlInRender = 0;
    if (got != n) shortRenders++;  // playback ended (empty project): rest is silence
    if (hash)
      for (int i = 0; i < n * 2; ++i) {
        uint32_t bits;
        memcpy(&bits, &buf[i], 4);
        for (int b = 0; b < 4; ++b) { h ^= (bits >> (b * 8)) & 0xff; h *= 1099511628211ull; }
      }
    frames -= n;
  }
  long long afterRender = gLive.load();
  long long peak = gPeak.load();
  (void)trapAudio; (void)shortRenders;

  if (csv)
    printf("%s,%s,%d,%.2f,%.2f,%.2f,%.2f,%.1f,%lld,%016llx\n", modeName, project == "file" ? path : project.c_str(),
           rate, mbf(afterCreate - baseLive), mbf(afterInit - baseLive), mbf(afterRender - baseLive),
           mbf(peak - baseLive), rssKb() / 1024.0, gRenderAllocs.load(), (unsigned long long)h);
  else {
    printf("mode=%s project=%s rate=%d\n", modeName, project == "file" ? path : project.c_str(), rate);
    printf("  heap after create      %7.2f MB\n", mbf(afterCreate - baseLive));
    printf("  heap after InitChips   %7.2f MB\n", mbf(afterInit - baseLive));
    printf("  heap after render      %7.2f MB\n", mbf(afterRender - baseLive));
    printf("  heap high-water        %7.2f MB   (includes loaded project data %.2f MB)\n",
           mbf(peak - baseLive), mbf(projectBytes));
    printf("  RSS                    %7.1f MB\n", rssKb() / 1024.0);
    printf("  allocations inside chipnomadRender: %lld\n", gRenderAllocs.load());
    if (hash) printf("  render hash %016llx\n", (unsigned long long)h);
  }
  (void)mb;
  chipnomadDestroy(state);
  return gRenderAllocs.load() && gTrap ? 2 : 0;
}
