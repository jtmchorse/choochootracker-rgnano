#ifndef CHOOCHOO_INSERT_FX_H
#define CHOOCHOO_INSERT_FX_H
#include <stddef.h>
#include <stdint.h>
extern "C++" {
#include "beads_pool.h"
enum InsertModule : uint8_t {
  insertOff,
  insertCompressor,
  insertDistortion,
  insertDoubler,
  insertTape,
  insertOTT,
  insertChorus,
  insertFlanger,
  insertPhaser,
  insertRotary,
  insertSaturation,
  insertBitcrusher,
  insertDestruction,
  insertParametricEQ,
  insertTranceGate,
  insertBeads,
  insertFormant,
  insertModuleCount
};
enum class InsertMapping : uint8_t { linear, exponential, bipolar, discrete };
struct InsertParameter {
  const char* label;
  const char* name;
  const char* units;
  float minimum, maximum;
  uint8_t initial;
  InsertMapping mapping;
};
struct InsertDescriptor {
  const char* name;
  uint8_t count;
  InsertParameter parameters[8];
};
struct InsertConfig {
  uint8_t module, bypass, values[8];
  // UI edit tokens cross the project snapshot but are never serialized.
  uint32_t edits[8], selection;
};
struct InsertAutomation {
  uint8_t values[2][8], valid[2];
};
const InsertDescriptor& insertDescriptor(int module);
int insertParameterMaximum(int module, int parameter);
bool insertParameterActive(int module, int parameter, int mode);
bool insertParameterModulatable(int module, int parameter, int mode);
uint8_t insertClamp(int module, int parameter, int value);
float insertMap(int module, int parameter, int value);
void insertSelect(InsertConfig*, int module);
void insertEdit(InsertConfig*, int parameter, int value);
void insertDescribe(char*, size_t, int module, int parameter, int value);
bool tranceGatePatternOpen(uint8_t pattern, uint8_t length, uint32_t step);
float tranceGateTicksPerStep(uint8_t rate);
class InsertChain {
 public:
  explicit InsertChain(float sampleRate);
  ~InsertChain();
  InsertChain(const InsertChain&) = delete;
  InsertChain& operator=(const InsertChain&) = delete;
  bool ready() const;
  // Audio-thread attachment of a producer-prepared lease, before sync().
  void attachBeads(int slot, const BeadsPool::Handle&);
  void reset();
  // The tracker clock is 24 ticks per quarter note. Tempo is updated at its
  // frame boundary, while trigger() aligns track gates with note-on events.
  void setTempo(float tickRate);
  void trigger();
  uint16_t sync(const InsertConfig config[2], InsertAutomation*, bool rendered = true);
  bool active() const;
  // Live-audio engines only (never offline export): arms the overload governor,
  // which bypasses the costliest slot when the effects alone would take most of
  // the CPU. See fx_profile.h.
  void setRealtime(bool);
  // Diagnostics: the largest absolute input/output sample a slot has seen since the
  // last call with reset=true (before the dry/wet fade, after clamping to +/-32).
  void peaks(int slot, float* input, float* output, bool reset);
  void process(float* interleaved, int frames, const uint8_t effective[2][8],
               const float* sidechain[2] = nullptr);

 private:
  struct Impl;
  Impl* impl_;
};
}
#endif
