#ifndef CHOOCHOO_FX_PROFILE_H
#define CHOOCHOO_FX_PROFILE_H
#include <stdint.h>

// Effect CPU bookkeeping and the "FX lite" switch.
//
// FX lite (default ON for RGNANO_BUILD, OFF elsewhere) groups the RG Nano
// savings that are not bit-identical to the full-quality path: silence/tail
// bypass of inserts and the master reverb, and the cheaper Beads engine
// (native-rate digital media, no 48 kHz resampling, no idle internal reverb).
// With lite OFF every effect renders exactly as before.
//
// The switch is read when an effect starts or resets (Beads engine choice) and
// once per block (bypass), so flipping it mid-song is safe; the Beads engine
// change takes effect the next time playback starts or the insert is reset.
void fxLiteSet(bool enabled);
bool fxLiteEnabled();

// Per-callback cost accumulated by effect type. Counters are written by the
// audio thread only; chipnomadGetFxProfile may be called from any thread.
struct FxProfile {
  enum { insertSlots = 17 };  // indexed by InsertModule (insertFormant = 16); a static_assert in fx_profile.cpp keeps it in step
  uint64_t insertNs[insertSlots];
  uint64_t reverbNs;       // master Clouds reverb send
  uint64_t beadsReverbNs;  // master Beads-algorithm reverb send
  uint64_t delayNs;        // master delay send (tape echo and the spring tank run inside it)
  uint64_t skippedFrames;  // insert frames bypassed by the silence skip
  uint64_t processedFrames;
  // Overload governor (live engines only): how many times an insert was
  // bypassed because the effects were taking more than ~70% of a core, and the
  // InsertModule that was bypassed last. Counters are never reset.
  uint32_t shedEvents;
  uint32_t shedModule;
};
// Copies the totals accumulated since the last reset. With reset=true the
// counters restart from zero, so the caller gets per-window totals.
void chipnomadGetFxProfile(FxProfile* out, bool reset);
// Cheap polling for the UI: the overload governor's event count and last module.
uint32_t fxShedEvents();
uint32_t fxShedModule();

// Formats one window as "<effect>=<us per callback> ... total=<us> skip=<%>",
// listing only effects that ran. `callbacks` is the number of audio callbacks
// in the window. Returns the length written.
//
// TODO(orchestrator): add this to the RG Nano "astat" log in
// tracker/platforms/sdl2/rgnano_platform.cpp, next to the "astat window" line:
//     FxProfile fp; chipnomadGetFxProfile(&fp, true);   // true = restart window
//     char fx[200]; fxProfileFormat(fp, cb, fx, sizeof fx);
//     rgnanoLog("astat fx us/cb: %s", fx);
// e.g. "astat fx us/cb: ott=1650 dist=820 beads=5100 rev=300 total=7900 skip=12%".
// Call it once per window with the same `cb` count as the render_us average
// (use reset=true so the next window starts clean). Not wired here because
// that file is owned by the limiter work.
// Short log name of an InsertModule id ("?" when out of range).
const char* fxProfileModuleName(int module);
int fxProfileFormat(const FxProfile& profile, int callbacks, char* out, unsigned size);

// Internal hooks used by the effect code.
uint64_t fxProfileNowNs();
void fxProfileInsert(int module, uint64_t ns, int processed, int skipped);
void fxProfileMaster(int which, uint64_t ns);  // 0 reverb, 1 beads reverb, 2 delay
void fxProfileShed(int module);
// Test hook: the share of real time the effects may use before the governor
// bypasses one (default 0.70).
void fxGovernorSetLimit(double fractionOfRealTime);
#endif
