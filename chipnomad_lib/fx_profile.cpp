#include "fx_profile.h"
#include <atomic>
#include <stdio.h>
#include <time.h>

namespace {
#ifdef RGNANO_BUILD
std::atomic<bool> lite{true};
#else
std::atomic<bool> lite{false};
#endif
// Single writer (audio thread); relaxed load+store avoids a locked RMW.
struct Counter {
  std::atomic<uint64_t> value{0};
  void add(uint64_t n) { value.store(value.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
  uint64_t take(bool reset) {
    uint64_t v = value.load(std::memory_order_relaxed);
    // Reader-side reset can lose one concurrent add; harmless for diagnostics.
    if (reset) value.store(0, std::memory_order_relaxed);
    return v;
  }
};
Counter insertNs[FxProfile::insertSlots], masterNs[3], skipped, processed;
std::atomic<uint32_t> shedEvents{0}, shedModule{0};
}  // namespace

void fxLiteSet(bool enabled) { lite.store(enabled, std::memory_order_relaxed); }
bool fxLiteEnabled() { return lite.load(std::memory_order_relaxed); }

uint64_t fxProfileNowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void fxProfileInsert(int module, uint64_t ns, int processedFrames, int skippedFrames) {
  if (module >= 0 && module < FxProfile::insertSlots) insertNs[module].add(ns);
  processed.add((uint64_t)processedFrames);
  skipped.add((uint64_t)skippedFrames);
}
void fxProfileMaster(int which, uint64_t ns) {
  if (which >= 0 && which < 3) masterNs[which].add(ns);
}
void fxProfileShed(int module) {
  shedModule.store((uint32_t)module, std::memory_order_relaxed);
  shedEvents.fetch_add(1, std::memory_order_relaxed);
}
uint32_t fxShedEvents() { return shedEvents.load(std::memory_order_relaxed); }
uint32_t fxShedModule() { return shedModule.load(std::memory_order_relaxed); }
void chipnomadGetFxProfile(FxProfile* out, bool reset) {
  if (!out) return;
  for (int i = 0; i < FxProfile::insertSlots; ++i) out->insertNs[i] = insertNs[i].take(reset);
  out->reverbNs = masterNs[0].take(reset);
  out->beadsReverbNs = masterNs[1].take(reset);
  out->delayNs = masterNs[2].take(reset);
  out->skippedFrames = skipped.take(reset);
  out->processedFrames = processed.take(reset);
  out->shedEvents = shedEvents.load(std::memory_order_relaxed);
  out->shedModule = shedModule.load(std::memory_order_relaxed);
}

int fxProfileFormat(const FxProfile& p, int callbacks, char* out, unsigned size) {
  static const char* const names[FxProfile::insertSlots] = {
      "off", "comp", "dist", "dbl", "tape", "ott", "chor", "flng", "phsr",
      "rot", "sat", "crush", "destr", "eq", "gate", "beads"};
  if (!out || size == 0) return 0;
  out[0] = 0;
  if (callbacks < 1) callbacks = 1;
  unsigned used = 0;
  uint64_t total = 0;
  auto add = [&](const char* name, uint64_t ns) {
    total += ns;
    if (!ns || used + 24 >= size) return;
    used += (unsigned)snprintf(out + used, size - used, "%s=%u ", name, (unsigned)(ns / 1000 / (uint64_t)callbacks));
  };
  for (int i = 1; i < FxProfile::insertSlots; ++i) add(names[i], p.insertNs[i]);
  add("rev", p.reverbNs);
  add("brev", p.beadsReverbNs);
  add("dly", p.delayNs);
  const uint64_t frames = p.skippedFrames + p.processedFrames;
  if (used + 40 < size)
    used += (unsigned)snprintf(out + used, size - used, "total=%u skip=%u%%", (unsigned)(total / 1000 / (uint64_t)callbacks),
                               frames ? (unsigned)(p.skippedFrames * 100 / frames) : 0u);
  if (p.shedEvents && used + 24 < size)
    used += (unsigned)snprintf(out + used, size - used, " shed=%u(last %s)", (unsigned)p.shedEvents,
                               p.shedModule < FxProfile::insertSlots ? names[p.shedModule] : "?");
  return (int)used;
}
