#include "audio_monitor.h"
#include "chipnomad_lib.h"
#include "soundbank_fx.h"
#include "chipnomad_lib_live_stick.h"
#include "playback.h"
#include "synth/braids_voice.h"
#include "synth/sample_voice.h"
#include "synth/soundbank_voice.h"
#include "synth/scwf_voice.h"
#undef LUT_FM_FREQUENCY_QUANTIZER
#undef LUT_FM_FREQUENCY_QUANTIZER_SIZE
#include "synth/plaits_voice.h"
#include "synth/plaits_alt_voice.h"
#include "synth/achchid_voice.h"
#include "synth/drum_synth_voice.h"
#include "synth/mme_voice.h"
#include "synth/sintered_voice.h"
#include "synth/opll_voice.h"
#include "synth/sid_voice.h"
#include "synth/dx7_voice.h"
#include "dx7_patch.h"
#include "opll_presets.h"
#include "opl_patch.h"
#include "simple_chip_presets.h"
#include "synth/simple_chip_voice.h"
#include "synth/opl_voice.h"
#include "synth/four_op_voice.h"
#include "synth/master_effects.h"
#include "synth/master_limiter.h"
#include "midi/midi_router.h"
#include <math.h>
#include <algorithm>
#include <atomic>
#include <new>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void detectAYPitchConflicts(ChipNomadState* state);
static void updateBraidsVoices(ChipNomadState* state);
static void updateSampleVoices(ChipNomadState* state);
static void updateSoundbankVoices(ChipNomadState* state);
static void updateSCWFVoices(ChipNomadState* state);
static void updatePlaitsVoices(ChipNomadState* state);
static void updatePlaitsAltVoices(ChipNomadState* state);
static void updateAChChidVoices(ChipNomadState* state);
static void updateDrumSynthVoices(ChipNomadState* state);
static void updateMMEVoices(ChipNomadState* state);
static void updateSinteredVoices(ChipNomadState* state);
static void updateOPLLVoices(ChipNomadState* state);
static void updateDX7Voices(ChipNomadState* state);
static void updateFourOpVoices(ChipNomadState* state);
static void updateOPLVoices(ChipNomadState* state);
static void updateSimpleChipVoices(ChipNomadState* state);
static void updateSIDVoices(ChipNomadState* state);
static void applyVoiceEvents(ChipNomadState* state, uint64_t dueMicros);
static int hasAudioRateModulation(const ChipNomadState* state);
static void updateAudioRateModulations(ChipNomadState* state);
static void motionRecordFrame(ChipNomadState* state);
static int instrumentFXCutoff(uint8_t value);
static int slewEngineFX(PlaybackTrackState*, FX, int);
static void updateInsertValues(ChipNomadState*);

// --- lazy voice support (see ChipNomadVoiceMode in chipnomad_lib.h) --------
template <typename T> static inline void killIf(T* p) { if (p) p->kill(); }
// Slot 0 of a (track, family) voice set is the publication gate: the UI thread
// fills slots 1..3, then stores slot 0 (seq_cst). Audio code only touches a set
// after seeing slot 0 non-NULL.
template <typename T> static inline T* gateLoad(T* const* slot) {
  return __atomic_load_n(const_cast<T**>(slot), __ATOMIC_SEQ_CST);
}

class AudioCommandQueue {
 public:
  bool hasSoundbank(uint32_t handle) const {
    for (const auto& slot : projectSlots_)
      if (slot.state.load() != kFree)
        for (const auto& i : slot.value.project.instruments)
          if (i.type == InstrumentType::SoundFont && i.chip.soundfont.prepared == handle)
            return true;
    return false;
  }
  void requestStop() { stopRequested_.store(1, std::memory_order_release); }

  int takeStopRequest() { return stopRequested_.exchange(0, std::memory_order_acq_rel); }

  int pushProject(const Project& project) {
    return publishProject(project);
  }

  int pushScale(uint8_t root, ScalePreset preset) {
    return pushCommand(kSetScale, root, preset);
  }

  void discardProject() {
    int old = projectPublished_.exchange(-1, std::memory_order_acq_rel);
    if (old >= 0) releaseProject(old);
    beadsPool_.collect();
  }

  void applyProject(ChipNomadState* state) {
    // Exchange transfers ownership of this publication exactly once. Claiming
    // by slot state first can mistake a recycled slot for an older publication
    // and strand a stale index in the mailbox; no retry loop belongs here.
    int slot = projectPublished_.exchange(-1, std::memory_order_acq_rel);
    if (slot < 0) return;
    projectSlots_[slot].state.store(kReading, std::memory_order_relaxed);
    auto& snapshot = projectSlots_[slot].value;
    attachBeads(state, snapshot);
    state->audioProject = snapshot.project;
    for (auto& handle : snapshot.beads) handle = {};
    projectSlots_[slot].state.store(kFree, std::memory_order_release);
  }

  // Called with audio stopped, after old chains have released their leases.
  void initializeBeads(ChipNomadState* state) {
    discardProject();
    beadsSampleRate_ = (float)state->sampleRate;
    BeadsPool::Handle handles[BeadsPool::capacity];
    prepareBeads(state->audioProject, handles);
    attachBeads(state, state->audioProject, handles);
  }

  void collectBeads() { beadsPool_.collect(); }

  int pushTrackEnabled(const uint8_t enabled[PROJECT_MAX_TRACKS]) {
    uint64_t mask = 0;
    for (int i = 0; i < PROJECT_MAX_TRACKS; ++i)
      if (enabled[i]) mask |= UINT64_C(1) << i;
    settingsDraft_.trackMask = mask;
    return publishSettings();
  }

  void pushLoopRange(LoopRange range) {
    settingsDraft_.loopRange = range;
    settingsDraft_.loopDirty = 1;
    publishSettings();
  }

  void clearLoopRange() {
    memset(&settingsDraft_.loopRange, 0, sizeof(settingsDraft_.loopRange));
    settingsDraft_.loopDirty = 1;
    publishSettings();
  }

  int pushCommand(uint8_t type, int a = 0, int b = 0, int c = 0, int d = 0,
                  const PhraseRow* row = NULL, const InstrumentOPLL* patch = NULL, const InstrumentOPL* opl = NULL, const InstrumentSimpleChip* simple = NULL, const InstrumentDX7* dx7 = NULL, const InstrumentFourOp* fourOp = NULL,const InstrumentSID* sid = NULL) {
    unsigned int head = commandHead_.load(std::memory_order_relaxed);
    unsigned int next = (head + 1) % kCommandCapacity;
    if (next == commandTail_.load(std::memory_order_acquire)) {
      commandOverflow_.fetch_add(1, std::memory_order_relaxed);
      return 0;
    }
    AudioCommand& command = commands_[head];
    command.type = type; command.a = a; command.b = b; command.c = c; command.d = d;
    if (row) command.row = *row;
    if (patch) command.patch = *patch;
    if (opl) command.opl = *opl;
    if (simple) command.simple = *simple;
    if (dx7) command.dx7 = *dx7;
    if (fourOp) command.fourOp = *fourOp;
    if (sid) command.sid = *sid;
    commandHead_.store(next, std::memory_order_release);
    return 1;
  }

  void applySettings(PlaybackState* playback) {
    int slot = claim(settingsSlots_, settingsPublished_);
    if (slot < 0) return;
    const Settings& settings = settingsSlots_[slot].value;
    {
      uint64_t mask = settings.trackMask;
      for (int i = 0; i < PROJECT_MAX_TRACKS; ++i)
        playback->trackEnabled[i] = (mask >> i) & 1;
    }
    if (settings.loopDirty) {
      if (settings.loopRange.enabled) playbackSetLoopRange(playback, settings.loopRange);
      else playbackClearLoopRange(playback);
    }
    settingsSlots_[slot].state.store(kFree, std::memory_order_release);
  }

  void applyCommands(ChipNomadState* state) {
    PlaybackState* playback = &state->playbackState;
    unsigned int tail = commandTail_.load(std::memory_order_relaxed);
    unsigned int head = commandHead_.load(std::memory_order_acquire);
    while (tail != head) {
      const AudioCommand& command = commands_[tail];
      switch (command.type) {
        case kStartSong: playbackStartSong(playback, command.a, command.b, command.c); break;
        case kStartChain: playbackStartChain(playback, command.a, command.b, command.c, command.d); break;
        case kStartPhrase: playbackStartPhrase(playback, command.a, command.b, command.c, command.d); break;
        case kStartPhraseRow: playbackStartPhraseRow(playback, command.a, const_cast<PhraseRow*>(&command.row)); break;
        case kStartPhraseRowFull:
          // Set the bypass AFTER playbackStartPhraseRow: resetTrack inside
          // clears the flag, so setting it first would be undone.
          playbackStartPhraseRow(playback, command.a, const_cast<PhraseRow*>(&command.row));
          playback->tracks[command.a].sliceBypass = 1;
          break;
        case kQueuePhrase: playbackQueuePhrase(playback, command.a, command.b, command.c); break;
        case kStartLiveChain: playbackStartLiveChain(playback, command.a, command.b); break;
        case kQueueLiveChain: playbackQueueLiveChain(playback, command.a, command.b, command.c); break;
        case kPreviewNote: playbackPreviewNote(playback, command.a, (uint8_t)command.b, (uint8_t)command.c); break;
        case 16:
          if(command.b&&!playbackIsPlaying(playback)&&state->sidPreview) {
            killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);killIf(state->opllPreview);killIf(state->oplPreview);killIf(state->simpleChipPreview);
            state->opllPreviewTrack=command.a;state->chipPreviewType=InstrumentType::SID;
            state->sidPreview->configure(&command.sid,6000,.7f);state->sidPreview->noteOn();
          }else if(!command.b){killIf(state->sidPreview);state->opllPreviewTrack=-1;}
          break;
        case 15:
          if(command.b&&!playbackIsPlaying(playback)&&state->fourOpPreview) {
            killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);killIf(state->opllPreview);killIf(state->oplPreview);killIf(state->simpleChipPreview);
            state->opllPreviewTrack=command.a;state->chipPreviewType=(InstrumentType)command.c;
            state->fourOpPreview->configure((InstrumentType)command.c,&command.fourOp,6000,.7f);state->fourOpPreview->noteOn();
          }else if(!command.b){killIf(state->sidPreview);killIf(state->fourOpPreview);state->opllPreviewTrack=-1;}
          break;
        case 14:
          if(command.b&&!playbackIsPlaying(playback)&&state->dx7Preview) {
            killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);killIf(state->opllPreview);killIf(state->oplPreview);killIf(state->simpleChipPreview);
            state->opllPreviewTrack=command.a;state->chipPreviewType=InstrumentType::DX7;
            state->dx7Preview->voices[0].configure(&command.dx7,6000,.7f);state->dx7Preview->voices[0].noteOn();
          }else if(!command.b){killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);state->opllPreviewTrack=-1;}
          break;
        case kSimplePreview:
          if(command.b && !playbackIsPlaying(playback)&&state->simpleChipPreview) {
            killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);killIf(state->opllPreview);killIf(state->oplPreview);killIf(state->simpleChipPreview);
            state->opllPreviewTrack=command.a;state->chipPreviewType=(InstrumentType)command.c;
            state->simpleChipPreview->configure((InstrumentType)command.c,&command.simple,6000,.7f);state->simpleChipPreview->noteOn();
          }else if(!command.b){killIf(state->simpleChipPreview);state->opllPreviewTrack=-1;}
          break;
        case kOPLPreview:
          if (command.b && !playbackIsPlaying(playback)&&state->oplPreview) {
            killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);killIf(state->opllPreview); killIf(state->oplPreview);killIf(state->simpleChipPreview);state->opllPreviewTrack=command.a;state->chipPreviewType=(InstrumentType)command.c;
            state->oplPreview->configure((InstrumentType)command.c,&command.opl,6000,.7f);state->oplPreview->noteOn();
          } else if (!command.b) {killIf(state->oplPreview);state->opllPreviewTrack=-1;}
          break;
        case kChipPreview:
          if (command.b && !playbackIsPlaying(playback)&&state->opllPreview) {
            killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);killIf(state->opllPreview); killIf(state->oplPreview);killIf(state->simpleChipPreview); state->chipPreviewType = InstrumentType::OPLL; state->opllPreviewTrack = command.a;
            state->opllPreview->configure(&command.patch, 6000, .7f); state->opllPreview->noteOn();
          } else if (!command.b) { killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);killIf(state->opllPreview); killIf(state->oplPreview);killIf(state->simpleChipPreview); state->opllPreviewTrack = -1; }
          break;
        case kStopPreview: playbackStopPreview(playback, command.a); break;
        case kClearTrackFX: memset(playback->tracks[command.a].note.fx, 0, sizeof(playback->tracks[command.a].note.fx)); break;
        case kSetScale:
          if (command.a >= 0 && command.a < 12 && command.b >= 0 && command.b < scalePresetCount) {
            playback->scaleRoot = (uint8_t)command.a;
            playback->scalePreset = (ScalePreset)command.b;
          }
          break;
      }
      tail = (tail + 1) % kCommandCapacity;
    }
    commandTail_.store(tail, std::memory_order_release);
  }

  void publishStatus(const PlaybackState* playback) {
    int old = statusPublished_.exchange(-1, std::memory_order_acq_rel);
    if (old >= 0) releasePublished(statusSlots_, old);
    int slot = findFree(statusSlots_);
    if (slot < 0) return;
    PlaybackStatus& status = statusSlots_[slot].value;
    memcpy(status.tracks, playback->tracks, sizeof(status.tracks));
    memcpy(status.trackEnabled, playback->trackEnabled, sizeof(status.trackEnabled));
    status.isPlaying = playbackIsPlaying(const_cast<PlaybackState*>(playback));
    statusSlots_[slot].state.store(kPublished, std::memory_order_release);
    statusPublished_.store(slot, std::memory_order_release);
  }

  int readStatus(PlaybackStatus* status) {
    int slot = claim(statusSlots_, statusPublished_);
    if (slot < 0) return 0;
    *status = statusSlots_[slot].value;
    statusSlots_[slot].state.store(kFree, std::memory_order_release);
    return 1;
  }

  int commandOverflow() const { return commandOverflow_.load(std::memory_order_relaxed) != 0; }
  void setRenderBufferOverflow() { renderBufferOverflow_.store(1, std::memory_order_relaxed); }
  int renderBufferOverflow() const { return renderBufferOverflow_.load(std::memory_order_relaxed); }

 private:
  enum { kFree, kPublished, kReading };
  template <typename T> struct Slot { T value; std::atomic<int> state{kFree}; };
  struct Settings { uint64_t trackMask = ~UINT64_C(0); LoopRange loopRange{}; uint8_t loopDirty = 0; };
  struct ProjectSnapshot {
    Project project;
    BeadsPool::Handle beads[BeadsPool::capacity];
  };
  struct AudioCommand { uint8_t type; int a, b, c, d; PhraseRow row; InstrumentOPLL patch; InstrumentOPL opl; InstrumentSimpleChip simple; InstrumentDX7 dx7; InstrumentFourOp fourOp; InstrumentSID sid; };
  // kStartPhraseRowFull is appended after upstream's preview commands so the
  // raw command IDs 14/15/16 used by applyCommands keep their meaning.
  enum CommandType { kStartSong, kStartChain, kStartPhrase, kStartPhraseRow, kQueuePhrase, kPreviewNote, kStopPreview, kClearTrackFX, kStartLiveChain, kQueueLiveChain, kSetScale, kChipPreview, kOPLPreview, kSimplePreview, kStartPhraseRowFull = 17 };
  static constexpr unsigned int kSlotCount = 3;
  static constexpr unsigned int kCommandCapacity = 64;

  template <typename T> static int findFree(Slot<T> slots[kSlotCount]) {
    for (unsigned int i = 0; i < kSlotCount; ++i) {
      int expected = kFree;
      if (slots[i].state.compare_exchange_strong(expected, kReading, std::memory_order_acq_rel)) return (int)i;
    }
    return -1;
  }
  template <typename T> static void releasePublished(Slot<T> slots[kSlotCount], int slot) {
    int expected = kPublished;
    slots[slot].state.compare_exchange_strong(expected, kFree, std::memory_order_acq_rel);
  }
  template <typename T> static int claim(Slot<T> slots[kSlotCount], std::atomic<int>& published) {
    int slot = published.load(std::memory_order_acquire);
    while (slot >= 0) {
      int expected = kPublished;
      if (slots[slot].state.compare_exchange_strong(expected, kReading, std::memory_order_acq_rel)) {
        int publishedSlot = slot;
        published.compare_exchange_strong(publishedSlot, -1, std::memory_order_acq_rel);
        return slot;
      }
      slot = published.load(std::memory_order_acquire);
    }
    return -1;
  }
  int publishProject(const Project& project) {
    int slot = findFree(projectSlots_);
    if (slot < 0) return 0;
    projectSlots_[slot].value.project = project;
    prepareBeads(project, projectSlots_[slot].value.beads);
    projectSlots_[slot].state.store(kPublished, std::memory_order_release);
    int old = projectPublished_.exchange(slot, std::memory_order_acq_rel);
    if (old >= 0) releaseProject(old);
    beadsPool_.collect();
    return 1;
  }
  void releaseProject(int slot) {
    int expected = kPublished;
    if (projectSlots_[slot].state.compare_exchange_strong(expected, kReading, std::memory_order_acq_rel)) {
      for (auto& handle : projectSlots_[slot].value.beads) handle = {};
      projectSlots_[slot].state.store(kFree, std::memory_order_release);
    }
  }
  void prepareBeads(const Project& project, BeadsPool::Handle* handles) {
    for (int chain = 0; chain <= PROJECT_MAX_TRACKS; ++chain) {
      const auto* config = chain == PROJECT_MAX_TRACKS ? project.masterInserts : project.trackInserts[chain];
      for (int slot = 0; slot < 2; ++slot) {
        int index = chain * 2 + slot;
        handles[index] = config[slot].module == insertBeads && beadsSampleRate_ > 0
            ? beadsPool_.prepare(index, beadsSampleRate_) : BeadsPool::Handle{};
      }
    }
  }
  static void attachBeads(ChipNomadState* state, const Project& project, const BeadsPool::Handle* handles) {
    for (int chain = 0; chain <= PROJECT_MAX_TRACKS; ++chain) {
      auto* target = chain == PROJECT_MAX_TRACKS ? state->masterInsertChain : state->insertChains[chain];
      const auto* config = chain == PROJECT_MAX_TRACKS ? project.masterInserts : project.trackInserts[chain];
      if (!target) continue;
      for (int slot = 0; slot < 2; ++slot) {
        // A removed Beads keeps its lease until resetDSP completes the fade.
        if (config[slot].module == insertBeads) target->attachBeads(slot, handles[chain * 2 + slot]);
      }
    }
  }
  static void attachBeads(ChipNomadState* state, const ProjectSnapshot& snapshot) {
    attachBeads(state, snapshot.project, snapshot.beads);
  }
  int publishSettings() {
    int old = settingsPublished_.exchange(-1, std::memory_order_acq_rel);
    if (old >= 0) releasePublished(settingsSlots_, old);
    int slot = findFree(settingsSlots_);
    if (slot < 0) return 0;
    settingsSlots_[slot].value = settingsDraft_;
    settingsSlots_[slot].state.store(kPublished, std::memory_order_release);
    settingsPublished_.store(slot, std::memory_order_release);
    return 1;
  }
  // Declared before snapshots so handles are destroyed before their pool.
  BeadsPool beadsPool_;
  float beadsSampleRate_ = 0;
  Slot<ProjectSnapshot> projectSlots_[kSlotCount];
  Slot<Settings> settingsSlots_[kSlotCount];
  Slot<PlaybackStatus> statusSlots_[kSlotCount];
  Settings settingsDraft_;
  std::atomic<int> stopRequested_{0};
  std::atomic<int> projectPublished_{-1};
  std::atomic<int> settingsPublished_{-1};
  std::atomic<int> statusPublished_{-1};
  AudioCommand commands_[kCommandCapacity] = {};
  std::atomic<unsigned int> commandHead_{0};
  std::atomic<unsigned int> commandTail_{0};
  std::atomic<int> commandOverflow_{0};
  std::atomic<int> renderBufferOverflow_{0};
};

static int16_t motionRecordLast[PROJECT_MAX_TRACKS][fxTotalCount][7];
static int motionRecordLastMode = -1;

static int slewEngineFX(PlaybackTrackState* track, FX fx, int target) {
  int index = (int)fx;
  if (track->slewTarget[index] < 0) {
    track->slewCurrent[index] = target;
    track->slewTarget[index] = target;
  } else if (track->slewTarget[index] != target) {
    track->slewTarget[index] = target;
    track->slewRemaining[index] = track->slewTicks;
  }
  if (!track->slewTicks) {
    track->slewCurrent[index] = target;
    track->slewRemaining[index] = 0;
  } else if (track->slewRemaining[index]) {
    int distance = target - track->slewCurrent[index];
    int step = distance / track->slewRemaining[index];
    if (!step && distance) step = distance < 0 ? -1 : 1;
    track->slewCurrent[index] += step;
    track->slewRemaining[index]--;
  }
  return track->slewCurrent[index];
}

static void resetMotionRecordLast(void) {
  for (int track = 0; track < PROJECT_MAX_TRACKS; ++track)
    for (int fx = 0; fx < fxTotalCount; ++fx)
      for(auto& v:motionRecordLast[track][fx])v=-1;
}

static int motionDestinationFX(ChipNomadState* state, int trackIdx, const Instrument* instrument, const PlaybackTrackState* track,
                               int destination, FX* fx, int* base, int* range,
                               InstrumentMotionValue* value,int* fmOperator = nullptr) {
  if(fmOperator)*fmOperator=0;
  int insert = instrumentGenericModDestination(instrument->type, destination) - genericModFirstInsert;
  if (insert >= 0 && insert < 16) {
    int slot = insert / 8, p = insert % 8;
    const auto& c = state->audioProject.trackInserts[trackIdx][slot];
    const auto& d = insertDescriptor(c.module);
    if (p >= d.count) return 0;
    *fx = (FX)(fxF11 + insert);
    *base = track->inserts.valid[slot] & (1 << p) ? track->inserts.values[slot][p] : c.values[p];
    *range = d.parameters[p].mapping == InsertMapping::discrete ? (int)d.parameters[p].maximum : 255;
    *value = InstrumentMotionValue::raw;
    return 1;
  }
  uint8_t rawFX;
  if (!instrumentMotionDestination(instrument, destination, &rawFX, base, range, value)) return 0;
  *fx = (FX)rawFX;
  if (*fx == fxTPN) *base = state->audioProject.trackPan[trackIdx];
  int direct,op;
  int generic=instrumentGenericModDestination(instrument->type,destination);
  if(nativeFMModTarget(generic,&direct,&op)&&direct>=fxOAR&&direct<=fxLEN) {
    int current=direct>=fxLFR?track->note.nativeFMCurrent.global[direct-fxLFR]:track->note.nativeFMCurrent.operators[op][direct-fxOAR];
    if(current)*base=current-1;
    if(fmOperator&&direct<=fxOE4)*fmOperator=op+1;
    return 1;
  }
  if (track->note.fx[*fx].isOn)
    *base = *value == InstrumentMotionValue::cutoff
      ? (int)instrumentFXCutoff(track->note.fx[*fx].fxValue)
      : track->note.fx[*fx].fxValue;
  return 1;
}

static int motionFXValue(int value, InstrumentMotionValue kind) {
  if (kind == InstrumentMotionValue::speed) return clampInt(value, 0, 500) * 255 / 500;
  if (kind == InstrumentMotionValue::cutoff) return filterControlFromCutoff((float)clampInt(value, FILTER_CUTOFF_MIN_HZ, FILTER_CUTOFF_MAX_HZ));
  return clampInt(value, 0, 255);
}

static int motionPhraseLocation(ChipNomadState* state, int trackIdx, uint16_t* phrase, uint8_t* row) {
  PlaybackTrackState* track = &state->playbackState.tracks[trackIdx];
  Project* project = &state->audioProject;
  if (track->mode == PlaybackMode::stopped || track->mode == PlaybackMode::phraseRow ||
      track->songRow < 0 || track->songRow >= PROJECT_MAX_LENGTH ||
      track->chainRow < 0 || track->chainRow >= PROJECT_MAX_LENGTH ||
      track->phraseRow < 0 || track->phraseRow >= 16) return 0;
  uint16_t chain = project->song[track->songRow][trackIdx];
  if (chain == EMPTY_VALUE_16 || chain >= PROJECT_MAX_CHAINS) return 0;
  *phrase = project->chains[chain].rows[track->chainRow].phrase;
  if (*phrase == EMPTY_VALUE_16 || *phrase >= PROJECT_MAX_PHRASES) return 0;
  *row = (uint8_t)track->phraseRow;
  return 1;
}

static void rebaseMotionRecordRate(ChipNomadState* state) {
  for (int trackIdx = 0; trackIdx < state->audioProject.tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &state->playbackState.tracks[trackIdx];
    if (track->note.instrument == EMPTY_VALUE_8) continue;
    Instrument* instrument = &state->audioProject.instruments[track->note.instrument];
    for (int slot = 0; slot < 4; ++slot) {
      PlaybackModState* modulation = &track->note.modulation[slot];
      if (!modulation->modulation || modulation->modulation->type != ModulationType::StickRate) continue;
      FX fx;
      int base, range, fmOperator=0; InstrumentMotionValue value;
      if (!motionDestinationFX(state, trackIdx, instrument, track, modulation->modulation->destination, &fx, &base, &range, &value,&fmOperator)) continue;
      int delta = playbackModScaleToRange(modulation->outValue, range);
      if (range == 16384) delta /= 129;
      if(fx>=fxOAR&&fx<=fxLEN) {
        NativeFXInfo info{};if(!instrumentNativeFXInfo(instrument,fx,&info,std::max(0,fmOperator-1)))continue;
        int v=clampInt(base+delta,info.minimum,info.maximum)+1;
        if(fmOperator)track->note.nativeFM.operators[fmOperator-1][fx-fxOAR]=track->note.nativeFMCurrent.operators[fmOperator-1][fx-fxOAR]=v;
        else track->note.nativeFM.global[fx-fxLFR]=track->note.nativeFMCurrent.global[fx-fxLFR]=v;
      } else if (fx >= fxF11 && fx <= fxF28) {
        int a=fx-fxF11, slot=a/8, p=a%8;
        track->inserts.values[slot][p]=insertClamp(state->audioProject.trackInserts[trackIdx][slot].module,p,base+delta);
        track->inserts.valid[slot]|=1<<p;
      } else {
        track->note.fx[fx].isOn = 1;
        track->note.fx[fx].fxValue = motionFXValue(base + delta, value);
      }
      state->playbackState.liveStickRate[track->note.instrument][slot] = 0;
      modulation->outValue = 0;
    }
  }
}

static void motionRecordFrame(ChipNomadState* state) {
  if (chipnomadMotionTakeRateReset())
    rebaseMotionRecordRate(state);
  int mode = chipnomadMotionMode();
  if (mode != motionRecordLastMode) {
    resetMotionRecordLast();
    motionRecordLastMode = mode;
  }
  if (!mode) return;

  for (int trackIdx = 0; trackIdx < state->audioProject.tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &state->playbackState.tracks[trackIdx];
    if (track->note.instrument == EMPTY_VALUE_8) continue;
    uint16_t phrase;
    uint8_t row;
    if (!motionPhraseLocation(state, trackIdx, &phrase, &row)) continue;
    Instrument* instrument = &state->audioProject.instruments[track->note.instrument];

    FX targets[4];
    int values[4],operators[4];
    InstrumentMotionValue valueKinds[4];
    int targetCount = 0;
    for (int slot = 0; slot < 4; ++slot) {
      PlaybackModState* modulation = &track->note.modulation[slot];
      if (!modulation->modulation || !modulationIsLiveStick(modulation->modulation->type)) continue;
      FX fx;
      int base, range, fmOperator=0; InstrumentMotionValue value;
      if (!motionDestinationFX(state, trackIdx, instrument, track, modulation->modulation->destination, &fx, &base, &range, &value,&fmOperator)) continue;
      // The compact tracker command set records operator 1 only. Live
      // modulation keeps its fixed operator and full native parameter range.
      if (fx>=fxOAR && fx<=fxLEN &&
          (fmOperator>1 || !instrumentFXAvailableForInstrument(instrument,fx))) continue;
      int target = -1;
      for (int i = 0; i < targetCount; ++i) if (targets[i] == fx && operators[i]==fmOperator) target = i;
      int delta = playbackModScaleToRange(modulation->outValue, range);
      if (range == 16384) delta = delta / 129;
      if (target < 0) {
        target = targetCount++;
        targets[target] = fx;
        operators[target]=fmOperator;
        values[target] = base;
        valueKinds[target] = value;
      }
      values[target] += delta;
    }

    for (int target = 0; target < targetCount; ++target) {
      FX fx = targets[target];
      int fxValue = motionFXValue(values[target], valueKinds[target]);
      if (fx >= fxF11 && fx <= fxF28) {
        int a=fx-fxF11;
        fxValue=insertClamp(state->audioProject.trackInserts[trackIdx][a/8].module,a%8,values[target]);
      }
      if(fx>=fxFBR && fx<=fxLEN) {
        NativeFXInfo info{};
        if(instrumentNativeFXInfo(instrument,fx,&info,std::max(0,operators[target]-1)))fxValue=clampInt(fxValue,info.minimum,info.maximum);
      }
      if (mode == 1 && motionRecordLast[trackIdx][fx][operators[target]] == fxValue) continue;
      MotionRecordEvent event = {phrase, row, (FXId)fx, (uint8_t)fxValue, (uint8_t)(mode == 2)};
      if (!chipnomadMotionPushEvent(event)) {
        chipnomadMotionSetOverflow();
        continue;
      }
      motionRecordLast[trackIdx][fx][operators[target]] = (int16_t)fxValue;
    }
  }
}

static void captureVoiceMonitor(ChipNomadState* state, int trackIdx,
                                const float* samples, int frames,
                                int channels, float envelope) {
  VoiceMonitor* monitor = &state->voiceMonitors[trackIdx];
  for (int i = 0; i < VOICE_MONITOR_SAMPLES; ++i) {
    int frame = frames > 1 ? (i * (frames - 1)) / (VOICE_MONITOR_SAMPLES - 1) : 0;
    monitor->samples[i] = samples[frame * channels];
  }
  monitor->envelope = envelope;
  monitor->active = 1;
}

static int resizeMixBuffers(ChipNomadState* state, int requiredSize) {
  float* mixBuffer = (float*)malloc(requiredSize * sizeof(float));
  float* reverbBuffer = (float*)malloc(requiredSize * sizeof(float));
  float* delayBuffer = (float*)malloc(requiredSize * sizeof(float));
  float* insertBuffer = (float*)malloc((size_t)requiredSize * PROJECT_MAX_TRACKS * sizeof(float));
  if (!mixBuffer || !reverbBuffer || !delayBuffer || !insertBuffer) {
    free(insertBuffer);
    free(mixBuffer);
    free(reverbBuffer);
    free(delayBuffer);
    return 0;
  }

  free(state->mixBuffer);
  free(state->reverbBuffer);
  free(state->delayBuffer);
  free(state->insertBuffer);
  state->insertBuffer = insertBuffer;
  state->mixBuffer = mixBuffer;
  state->reverbBuffer = reverbBuffer;
  state->delayBuffer = delayBuffer;
  state->mixBufferSize = requiredSize;
  return 1;
}

static int instrumentFXCutoff(uint8_t value) {
  return (int)filterCutoffFromControl(value);
}

// Sends need to become audible earlier than master returns, while zero remains
// a true off state and full scale remains unity.
static float mixerSendGain(uint8_t value) {
  if (value == 0) return 0.0f;
  return powf(10.0f, -36.0f * (100 - value) / 2000.0f);
}

static float phraseGain(const PlaybackState* playback, const PlaybackTrackState* track,
                        const Instrument* instrument) {
  return instrument->volume * playbackVolumeGain(playback, track) / 255.0f;
}

static float effectiveTrackSend(ChipNomadState* state, int trackIdx,
                                bool reverb) {
  PlaybackTrackState* track = &state->playbackState.tracks[trackIdx];
  int value = reverb ? state->audioProject.trackReverbSend[trackIdx]
                     : state->audioProject.trackDelaySend[trackIdx];
  if (track->note.fx[reverb ? fxRSN : fxDSN].isOn)
    value = track->note.fx[reverb ? fxRSN : fxDSN].fxValue * 100 / 255;
  if (track->note.instrument != EMPTY_VALUE_8) {
    InstrumentType type = state->audioProject.instruments[track->note.instrument].type;
    for (int i = 0; i < 4; ++i) {
      PlaybackModState* mod = &track->note.modulation[i];
      if (!mod->modulation) continue;
      int destination = instrumentGenericModDestination(type,
        mod->modulation->destination);
      if (destination != (reverb ? genericModReverbSend : genericModDelaySend)) continue;
      int modulation = playbackModScaleToRange(mod->outValue, 100);
      value = (modulationIsAdditive(mod->modulation->type) ||
               mod->modulation->type == ModulationType::SLFO ||
               mod->modulation->type == ModulationType::FLFO)
        ? value + modulation : modulation;
    }
  }
  value = clampInt(value, 0, 100);
  return state->audioProject.perceptualEffects ? mixerSendGain((uint8_t)value) : value / 100.0f;
}

static int effectivePan(ChipNomadState* state, int trackIdx, bool instrumentPan) {
  PlaybackTrackState* track = &state->playbackState.tracks[trackIdx];
  int value = instrumentPan ? 128 : state->audioProject.trackPan[trackIdx];
  InstrumentType type = InstrumentType::none;
  if (track->note.instrument != EMPTY_VALUE_8) {
    const Instrument* instrument = &state->audioProject.instruments[track->note.instrument];
    type = instrument->type;
    if (instrumentPan) value = instrument->pan;
  }
  FX fx = instrumentPan ? fxPAN : fxTPN;
  if (track->note.fx[fx].isOn) value = track->note.fx[fx].fxValue;
  if (instrumentPan && type == InstrumentType::SoundFont && track->note.fx[fxFPN].isOn)
    value = track->note.fx[fxFPN].fxValue;
  for (const auto& mod : track->note.modulation) {
    if (!mod.modulation) continue;
    int destination = instrumentGenericModDestination(type, mod.modulation->destination);
    if (destination != (instrumentPan ? genericModInstrumentPan : genericModTrackPan)) continue;
    int offset = playbackModScaleToRange(mod.outValue, 255);
    value = modulationIsAdditive(mod.modulation->type) || mod.modulation->type == ModulationType::SLFO || mod.modulation->type == ModulationType::FLFO ? value + offset : offset;
  }
  return clampInt(value, 0, 255);
}

static float panGain(int pan, int channel) {
  // Centre is deliberately transparent so old projects remain bit-identical.
  // Pan values are bytes; never call the costly software trig functions from
  // the per-sample audio path.
  static float gains[256][2];
  static bool initialized = false;
  if (!initialized) {
    for (int value = 0; value < 256; ++value) {
      gains[value][0] = value <= 128 ? 1.0f : cosf((value - 128) * 1.57079632679f / 127.0f);
      gains[value][1] = value >= 128 ? 1.0f : sinf(value * 1.57079632679f / 128.0f);
    }
    initialized = true;
  }
  return gains[clampInt(pan, 0, 255)][channel];
}

struct TrackPanMix {
  float leftGain = 1.0f, rightGain = 1.0f, amount = 0.0f, attenuation = 1.0f;
  int direction = 0;
  bool active = false;
};

static TrackPanMix makeTrackPanMix(ChipNomadState* state, int instrumentPan, int trackPan) {
  TrackPanMix pan;
  if (state->benchmarkBypassPan || (instrumentPan == 128 && trackPan == 128)) return pan;
  pan.active = true;
  pan.leftGain = panGain(instrumentPan, 0);
  pan.rightGain = panGain(instrumentPan, 1);
  if (trackPan < 128) {
    pan.direction = -1;
    pan.amount = (128 - trackPan) / 128.0f;
  } else if (trackPan > 128) {
    pan.direction = 1;
    pan.amount = (trackPan - 128) / 127.0f;
  }
  pan.attenuation = 1.0f - pan.amount;
  return pan;
}

static inline void mixTrackChannel(ChipNomadState* state, int trackIdx,
                                   float* mix, float* reverb, float* delay,
                                   float sample, int sampleIndex, float reverbSend,
                                   float delaySend) {
  int channel = sampleIndex & 1;
  TrackTilt& tilt = state->trackTilt[trackIdx];
  const uint8_t tiltValue = state->audioProject.trackTilt[trackIdx];
  sample = tilt.isNeutral(tiltValue, state->audioProject.tiltPivotHz)
    ? tilt.passThrough(sample, channel)
    : tilt.process(sample, channel, tiltValue, state->audioProject.tiltPivotHz);
  if (state->insertActive[trackIdx]) {
    state->insertBuffer[(size_t)trackIdx * state->mixBufferSize + sampleIndex] += sample;
    return;
  }
  state->audioMonitor->add(trackIdx, sampleIndex, sample);
  float previous = *mix;
  *mix += sample;
  if (reverbSend != 0.0f) *reverb += sample * reverbSend;
  if (delaySend != 0.0f) *delay += sample * delaySend;
  if (fabsf(*mix) > 1.0f && fabsf(*mix) > fabsf(previous)) {
    state->trackClipping[trackIdx] = AUDIO_OVERLOAD_COOLDOWN_FRAMES;
  }
}

static inline void mixTrackFrame(ChipNomadState* state, int trackIdx,
                                 float* mix, float* reverb, float* delay,
                                 float left, float right, int frameIndex,
                                 float reverbSend, float delaySend,
                                 const TrackPanMix& pan) {
  if (pan.active) {
    left *= pan.leftGain;
    right *= pan.rightGain;
    if (pan.direction < 0) {
      left += (right - left) * pan.amount * 0.5f;
      right *= pan.attenuation;
    } else if (pan.direction > 0) {
      right += (left - right) * pan.amount * 0.5f;
      left *= pan.attenuation;
    }
  }
  int sampleIndex = frameIndex * 2;
  mixTrackChannel(state, trackIdx, &mix[sampleIndex], &reverb[sampleIndex], &delay[sampleIndex],
                  left, sampleIndex, reverbSend, delaySend);
  mixTrackChannel(state, trackIdx, &mix[sampleIndex + 1], &reverb[sampleIndex + 1], &delay[sampleIndex + 1],
                  right, sampleIndex + 1, reverbSend, delaySend);
}


// ---------------------------------------------------------------------------
// Lazy voice allocation
//
// Threading model
//   UI thread   : the only thread that creates or destroys voice objects
//                 (chipnomadCreateEx/InitChips/PrepareVoices/Queue*). It fills
//                 slots 1..3 of a set, then publishes with a seq_cst store to
//                 slot 0. A set is retired by a seq_cst store of NULL to slot 0;
//                 the objects are deleted only after the audio thread has left
//                 any chipnomadRender() call that began before the store
//                 (renderSeq is odd while a render call runs).
//   audio thread: never allocates or frees. It reads slot 0 with a seq_cst
//                 load; NULL means "not built": it renders silence for that
//                 track/family and ORs the family into state->voiceMissing,
//                 which chipnomadGetPlaybackStatus (UI thread) turns into a
//                 rebuild. In lazyOffline mode the "audio" thread is the
//                 caller's own thread, so the render call builds the set.
// ---------------------------------------------------------------------------
enum VoiceFamily {
  vfBraids, vfSoundbank, vfSample, vfSCWF, vfPlaits, vfPlaitsAlt, vfAChChid, vfDrumSynth,
  vfMME, vfSintered, vfOPLL, vfOPL, vfFourOp, vfSimpleChip, vfDX7, vfSID, vfCount
};

static int voiceFamilyForType(InstrumentType type) {
  switch (type) {
    case InstrumentType::Braids: return vfBraids;
    case InstrumentType::SoundFont: return vfSoundbank;
    case InstrumentType::Sample: return vfSample;
    case InstrumentType::SCWF: case InstrumentType::BYOWTBL: return vfSCWF;
    case InstrumentType::Plaits: return vfPlaits;
    case InstrumentType::PlaitsAlt: return vfPlaitsAlt;
    case InstrumentType::AChChid: return vfAChChid;
    case InstrumentType::DrumSynth: return vfDrumSynth;
    case InstrumentType::MME: return vfMME;
    case InstrumentType::Sintered: return vfSintered;
    case InstrumentType::OPLL: case InstrumentType::VRC7: return vfOPLL;
    case InstrumentType::OPL2: case InstrumentType::OPL3: return vfOPL;
    case InstrumentType::GenesisFM: case InstrumentType::ArcadeFM: return vfFourOp;
    case InstrumentType::SegaPSG: case InstrumentType::GBPulse: case InstrumentType::GBNoise: return vfSimpleChip;
    case InstrumentType::DX7: return vfDX7;
    case InstrumentType::SID: return vfSID;
    default: return -1;
  }
}

// X(family, VoiceType, array) for every family that is a plain pointer array.
#define CN_VOICE_FAMILIES(X) \
  X(vfBraids, BraidsVoice, braidsVoices) X(vfSoundbank, SoundbankVoice, soundbankVoices) \
  X(vfSample, SampleVoice, sampleVoices) X(vfSCWF, SCWFVoice, scwfVoices) \
  X(vfPlaits, PlaitsVoice, plaitsVoices) X(vfPlaitsAlt, PlaitsAltVoice, plaitsAltVoices) \
  X(vfAChChid, AChChidVoice, achchidVoices) X(vfDrumSynth, DrumSynthVoice, drumSynthVoices) \
  X(vfMME, MMEVoice, mmeVoices) X(vfSintered, SinteredVoice, sinteredVoices) \
  X(vfOPLL, OPLLVoice, opllVoices) X(vfOPL, OPLVoice, oplVoices) \
  X(vfFourOp, FourOpVoice, fourOpVoices) X(vfSimpleChip, SimpleChipVoice, simpleChipVoices) \
  X(vfSID, SIDVoice, sidVoices)

static inline bool voiceFamilyReady(const ChipNomadState* s, int t, int fam) {
  switch (fam) {
#define X(F, T, A) case F: return gateLoad(&s->A[t][0]) != nullptr;
    CN_VOICE_FAMILIES(X)
#undef X
    case vfDX7: return gateLoad(&s->dx7Parts[t]) != nullptr;
  }
  return true;
}

// Audio thread: record that a track met a missing family.
static inline void voiceFlagMissing(ChipNomadState* s, int t, int fam) {
  __atomic_or_fetch(&s->voiceMissing[t], 1u << fam, __ATOMIC_RELAXED);
}

static inline uint32_t voiceSeq(const ChipNomadState* s) {
  return __atomic_load_n(&s->renderSeq, __ATOMIC_SEQ_CST);
}

template <typename T> static void voiceDestroyOne(void* p) { delete static_cast<T*>(p); }

static void voiceRetire(ChipNomadState* s, void* object, void (*destroy)(void*)) {
  if (!object) return;
  if (s->voiceRetiredCount >= (int)(sizeof(s->voiceRetired) / sizeof(s->voiceRetired[0]))) {
    // Table full: nothing can safely be dropped, so leak until InitChips/Destroy
    // flushes. Only reachable if the audio thread is stuck inside one render.
    return;
  }
  s->voiceRetired[s->voiceRetiredCount++] = {object, destroy, voiceSeq(s)};
}

// UI thread. force=1 is for Destroy/InitChips, when audio is stopped.
static void voiceReclaim(ChipNomadState* s, bool force) {
  const uint32_t now = voiceSeq(s);
  int kept = 0;
  for (int i = 0; i < s->voiceRetiredCount; ++i) {
    auto e = s->voiceRetired[i];
    bool safe = force || (e.seq & 1u) == 0 || now != e.seq;
    if (safe) e.destroy(e.object);
    else s->voiceRetired[kept++] = e;
  }
  s->voiceRetiredCount = kept;
}

// UI thread. Builds one (track, family) set at the engine's real sample rate.
static bool voiceCreateFamily(ChipNomadState* s, int t, int fam) {
  const float rate = (float)s->sampleRate;
  if (rate <= 0.0f || voiceFamilyReady(s, t, fam)) return rate > 0.0f;
  try {
    switch (fam) {
#define X(F, T, A) \
      case F: { \
        T* made[CHORD_MAX_VOICES] = {}; \
        bool ok = true; \
        for (int v = 0; v < CHORD_MAX_VOICES && ok; ++v) { \
          made[v] = new (std::nothrow) T(); \
          if (made[v]) made[v]->init(rate); else ok = false; \
        } \
        if (!ok) { for (auto* m : made) delete m; s->voiceAllocFailures++; return false; } \
        for (int v = 1; v < CHORD_MAX_VOICES; ++v) s->A[t][v] = made[v]; \
        __atomic_store_n(&s->A[t][0], made[0], __ATOMIC_SEQ_CST); \
        break; }
      CN_VOICE_FAMILIES(X)
#undef X
      case vfDX7: {
        DX7Part* part = new (std::nothrow) DX7Part();
        if (!part) { s->voiceAllocFailures++; return false; }
        part->init(rate);
        for (int v = 0; v < CHORD_MAX_VOICES; ++v) s->dx7Voices[t][v] = &part->voices[v];
        __atomic_store_n(&s->dx7Parts[t], part, __ATOMIC_SEQ_CST);
        break;
      }
      default: return false;
    }
  } catch (...) {  // a voice's own init() ran out of memory
    s->voiceAllocFailures++;
    return false;
  }
  if (fam == vfBraids && s->braidsSettingsValid)
    for (int v = 0; v < CHORD_MAX_VOICES; ++v)
      s->braidsVoices[t][v]->setGlobalSettings(s->braidsBits, s->braidsDrift, s->braidsSignature, s->braidsSeed);
  s->voiceMask[t] |= 1u << fam;
  return true;
}

static bool voiceFamilyIdle(const ChipNomadState* s, int t, int fam) {
  switch (fam) {
#define X(F, T, A) \
    case F: for (int v = 0; v < CHORD_MAX_VOICES; ++v) if (s->A[t][v] && s->A[t][v]->active()) return false; return true;
    CN_VOICE_FAMILIES(X)
#undef X
    case vfDX7: return !s->dx7Parts[t] || !s->dx7Parts[t]->active();
  }
  return true;
}

// UI thread. Unpublishes the set and queues the objects for deferred deletion.
// Slots 1..3 keep their (retired but still valid) pointers so a render call
// already past the gate never dereferences a freed or NULL slot.
static void voiceRetireFamily(ChipNomadState* s, int t, int fam) {
  switch (fam) {
#define X(F, T, A) \
    case F: { \
      T* old[CHORD_MAX_VOICES]; \
      for (int v = 0; v < CHORD_MAX_VOICES; ++v) old[v] = s->A[t][v]; \
      __atomic_store_n(&s->A[t][0], (T*)nullptr, __ATOMIC_SEQ_CST); \
      for (auto* o : old) voiceRetire(s, o, voiceDestroyOne<T>); \
      break; }
    CN_VOICE_FAMILIES(X)
#undef X
    case vfDX7: {
      DX7Part* old = s->dx7Parts[t];
      __atomic_store_n(&s->dx7Parts[t], (DX7Part*)nullptr, __ATOMIC_SEQ_CST);
      voiceRetire(s, old, voiceDestroyOne<DX7Part>);
      break;
    }
  }
  s->voiceMask[t] &= ~(1u << fam);
}

// Families each track can reach from its song: song cell -> chain -> phrase ->
// instrument. Phrase rows are the only place an instrument is chosen.
static void voiceNeedsFromProject(const Project* p, uint32_t needs[PROJECT_MAX_TRACKS]) {
  static const uint32_t unknown = 0xffffffffu;
  uint32_t phraseMask[PROJECT_MAX_PHRASES], chainMask[PROJECT_MAX_CHAINS];
  for (auto& m : phraseMask) m = unknown;
  for (auto& m : chainMask) m = unknown;
  for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) needs[t] = 0;
  const int tracks = std::min(p->tracksCount, (int)PROJECT_MAX_TRACKS);
  for (int row = 0; row < PROJECT_MAX_LENGTH; ++row) {
    for (int t = 0; t < tracks; ++t) {
      uint16_t chain = p->song[row][t];
      if (chain >= PROJECT_MAX_CHAINS) continue;
      if (chainMask[chain] == unknown) {
        uint32_t cm = 0;
        for (const auto& cr : p->chains[chain].rows) {
          if (cr.phrase >= PROJECT_MAX_PHRASES) continue;
          if (phraseMask[cr.phrase] == unknown) {
            uint32_t pm = 0;
            for (const auto& pr : p->phrases[cr.phrase].rows) {
              if (pr.instrument >= PROJECT_MAX_INSTRUMENTS) continue;
              int f = voiceFamilyForType(p->instruments[pr.instrument].type);
              if (f >= 0) pm |= 1u << f;
            }
            phraseMask[cr.phrase] = pm;
          }
          cm |= phraseMask[cr.phrase];
        }
        chainMask[chain] = cm;
      }
      needs[t] |= chainMask[chain];
    }
  }
}

// UI thread. Build what the project can reach; optionally drop idle leftovers.
static int voiceEnsure(ChipNomadState* s, const Project* p, bool prune) {
  if (s->voiceMode == ChipNomadVoiceMode::eager || s->sampleRate <= 0) return 0;
  uint32_t needs[PROJECT_MAX_TRACKS];
  voiceNeedsFromProject(p, needs);
  int failures = 0;
  for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) {
    const uint32_t want = needs[t] | s->voiceExtra[t];
    for (int f = 0; f < vfCount; ++f) {
      const bool wanted = (want >> f) & 1u, built = (s->voiceMask[t] >> f) & 1u;
      if (wanted && !built) { if (!voiceCreateFamily(s, t, f)) ++failures; }
      else if (prune && !wanted && built && voiceFamilyIdle(s, t, f)) voiceRetireFamily(s, t, f);
    }
  }
  voiceReclaim(s, false);
  return failures;
}

// UI thread. A direct command (preview, start phrase row) names an instrument
// that may not be reachable from the song: pin its family on that track.
static void voiceEnsureInstrument(ChipNomadState* s, int track, int instrument) {
  if (s->voiceMode == ChipNomadVoiceMode::eager || track < 0 || track >= PROJECT_MAX_TRACKS ||
      instrument < 0 || instrument >= PROJECT_MAX_INSTRUMENTS || s->sampleRate <= 0) return;
  int f = voiceFamilyForType(s->project.instruments[instrument].type);
  if (f < 0) return;
  s->voiceExtra[track] |= 1u << f;
  voiceCreateFamily(s, track, f);
}

// UI thread. The audio thread reported voices it needed but did not find
// (a project edit it saw before the UI refresh): build exactly those.
static void voiceServiceMissing(ChipNomadState* s) {
  if (s->voiceMode == ChipNomadVoiceMode::eager) return;
  for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) {
    uint32_t bits = __atomic_exchange_n(&s->voiceMissing[t], 0u, __ATOMIC_ACQ_REL);
    for (int f = 0; bits && f < vfCount; ++f)
      if ((bits >> f) & 1u) { s->voiceExtra[t] |= 1u << f; voiceCreateFamily(s, t, f); }
  }
  voiceReclaim(s, false);
}

// UI thread. Native chip previews are single objects built on first audition.
template <typename T> static T* previewEnsure(ChipNomadState* s, T*& slot) {
  if (!slot && s->sampleRate > 0) {
    T* made = new (std::nothrow) T();
    if (made) {
      try { made->init((float)s->sampleRate); } catch (...) { delete made; made = nullptr; }
    }
    if (made) __atomic_store_n(&slot, made, __ATOMIC_SEQ_CST);
    else s->voiceAllocFailures++;
  }
  return slot;
}

int chipnomadPrepareVoices(ChipNomadState* state) {
  if (!state) return 0;
  return voiceEnsure(state, &state->project, true);
}

ChipNomadVoiceMode chipnomadDefaultVoiceMode(void) {
#ifdef RGNANO_BUILD
  return ChipNomadVoiceMode::lazyRealtime;
#else
  return ChipNomadVoiceMode::eager;
#endif
}

// Audio thread, once per tick after the playback engine chose each track's
// instrument and before the voices are configured.
static void voiceAuditTracks(ChipNomadState* s) {
  const Project* p = &s->audioProject;
  for (int t = 0; t < p->tracksCount; ++t) {
    const int ins = s->playbackState.tracks[t].note.instrument;
    if (ins >= PROJECT_MAX_INSTRUMENTS) continue;
    const int f = voiceFamilyForType(p->instruments[ins].type);
    if (f < 0 || voiceFamilyReady(s, t, f)) continue;
    if (s->voiceMode == ChipNomadVoiceMode::lazyOffline) voiceCreateFamily(s, t, f);
    else voiceFlagMissing(s, t, f);
  }
}

static SoundChip* defaultChipFactory(int chipIndex, int sampleRate, ChipSetup setup) {
  setup.ay.stereoSeparation = 0;
  return new SoundChipAY(sampleRate, setup);
}

ChipNomadState* chipnomadCreate(void) {
  return chipnomadCreateEx(chipnomadDefaultVoiceMode());
}

ChipNomadState* chipnomadCreateEx(ChipNomadVoiceMode voiceMode) {
  ChipNomadState* state = (ChipNomadState*)malloc(sizeof(ChipNomadState));
  if (!state) return NULL;

  memset(state, 0, sizeof(ChipNomadState));
  state->voiceMode = voiceMode;
  state->ownsProjectResources = 1;
  state->audioCommands = new AudioCommandQueue();
  state->midiRouter = midiRouterCreate();
  fillFXNames();
  projectInit(&state->project);
  state->audioProject = state->project;
  playbackInit(&state->playbackState, &state->audioProject);
  state->mixVolume = 1.0f;
  state->aySampleDithering = 1; // Default: ON

  // Initialize mix buffer
  state->mixBufferSize = 8192;
  state->mixBuffer = (float*)malloc(state->mixBufferSize * sizeof(float));
  state->reverbBuffer = (float*)malloc(state->mixBufferSize * sizeof(float));
  state->delayBuffer = (float*)malloc(state->mixBufferSize * sizeof(float));
  state->insertBuffer = (float*)malloc((size_t)state->mixBufferSize * PROJECT_MAX_TRACKS * sizeof(float));
  if (!state->mixBuffer || !state->reverbBuffer || !state->delayBuffer || !state->insertBuffer) {
    free(state->insertBuffer);
    free(state->mixBuffer);
    free(state->reverbBuffer);
    free(state->delayBuffer);
    free(state);
    return NULL;
  }

  state->audioMonitor = new AudioMonitor();
  state->masterEffects = new MasterEffects();
  state->masterLimiter = new MasterLimiter();
  state->masterLimiterEnabled = 0;  // the app applies appSettings.masterLimiter; exports and tests stay bit-exact
  // Lazy modes build every voice, preview and the master delay line at the
  // real rate in chipnomadInitChips instead of at a throwaway 96 kHz here.
  if (voiceMode != ChipNomadVoiceMode::eager) return state;
  state->masterEffects->init(96000.0f);

  state->sidPreview=new SIDVoice();state->sidPreview->init(96000);
  state->simpleChipPreview=new SimpleChipVoice();state->simpleChipPreview->init(96000);
  state->fourOpPreview=new FourOpVoice();state->fourOpPreview->init(96000);
  state->oplPreview = new OPLVoice();state->oplPreview->init(96000);state->chipPreviewType=InstrumentType::none;
  state->dx7Preview=new DX7Part();state->dx7Preview->init(96000);
  state->opllPreview = new OPLLVoice();
  state->opllPreview->init(96000.0f); state->opllPreviewTrack = -1;
  for (int i = 0; i < PROJECT_MAX_TRACKS; i++) {
    for (int voice = 0; voice < CHORD_MAX_VOICES; ++voice) {
      state->braidsVoices[i][voice] = new BraidsVoice();
      state->braidsVoices[i][voice]->init();
      state->soundbankVoices[i][voice] = new SoundbankVoice();
      state->soundbankVoices[i][voice]->init(96000.0f);
      state->sampleVoices[i][voice] = new SampleVoice();
      state->sampleVoices[i][voice]->init(96000.0f);
      state->scwfVoices[i][voice] = new SCWFVoice();
      state->scwfVoices[i][voice]->init(96000.0f);
      state->plaitsVoices[i][voice] = new PlaitsVoice();
      state->plaitsVoices[i][voice]->init(96000.0f);
      state->plaitsAltVoices[i][voice] = new PlaitsAltVoice();
      state->plaitsAltVoices[i][voice]->init(96000.0f);
      state->achchidVoices[i][voice] = new AChChidVoice();
      state->achchidVoices[i][voice]->init(96000.0f);
      state->drumSynthVoices[i][voice] = new DrumSynthVoice();
      state->drumSynthVoices[i][voice]->init(96000.0f);
      state->mmeVoices[i][voice] = new MMEVoice();
      state->mmeVoices[i][voice]->init(96000.0f);
      state->sidVoices[i][voice]=new SIDVoice();state->sidVoices[i][voice]->init(96000);
      state->simpleChipVoices[i][voice]=new SimpleChipVoice();state->simpleChipVoices[i][voice]->init(96000);
      state->fourOpVoices[i][voice]=new FourOpVoice();state->fourOpVoices[i][voice]->init(96000);
      state->oplVoices[i][voice] = new OPLVoice();state->oplVoices[i][voice]->init(96000);
      if(!voice){state->dx7Parts[i]=new DX7Part();state->dx7Parts[i]->init(96000);}
      state->dx7Voices[i][voice]=&state->dx7Parts[i]->voices[voice];
      state->opllVoices[i][voice] = new OPLLVoice();
      state->opllVoices[i][voice]->init(96000.0f);
      state->sinteredVoices[i][voice] = new SinteredVoice();
      state->sinteredVoices[i][voice]->init(96000.0f);
    }
  }

  return state;
}

void chipnomadDestroy(ChipNomadState* state) {
  if (!state) return;

  // Cleanup chips
  for (int i = 0; i < PROJECT_MAX_CHIPS; i++) {
    if (state->chips[i]) {
      delete state->chips[i];
      state->chips[i] = nullptr;
    }
  }

  delete state->sidPreview;
  delete state->simpleChipPreview;
  delete state->fourOpPreview;
  delete state->oplPreview;
  delete state->dx7Preview;
  delete state->opllPreview;
  // Audio is stopped: reclaim retired sets first, then delete only the sets
  // whose gate is open (the slots of a retired set hold dangling pointers).
  voiceReclaim(state, true);
  for (int i = 0; i < PROJECT_MAX_TRACKS; i++) {
#define X(F, T, A) if (state->A[i][0]) for (int voice = 0; voice < CHORD_MAX_VOICES; ++voice) delete state->A[i][voice];
    CN_VOICE_FAMILIES(X)
#undef X
    delete state->dx7Parts[i];
  }

  if (state->ownsProjectResources) projectFree(&state->project);

  for (auto* chain : state->insertChains) delete chain;
  delete state->masterInsertChain;
  free(state->insertBuffer);
  // Cleanup mix buffer
  free(state->mixBuffer);
  free(state->reverbBuffer);
  free(state->delayBuffer);
  delete state->masterEffects;
  delete state->masterLimiter;
  delete state->audioCommands;
  midiRouterDestroy(state->midiRouter);
  delete state->audioMonitor;

  free(state);
}

void chipnomadInitChips(ChipNomadState* state, int sampleRate, ChipFactory factory) {
  if (!state) return;
  state->audioProject = state->project;
  state->playbackState.p = &state->audioProject;

  // Cleanup existing chips if already initialized
  if (state->sampleRate > 0) {
    for (int i = 0; i < PROJECT_MAX_CHIPS; i++) {
      if (state->chips[i]) {
        delete state->chips[i];
        state->chips[i] = nullptr;
      }
    }
  }

  // Zero the entire chips array for safety
  memset(state->chips, 0, sizeof(state->chips));
  state->sampleRate = sampleRate;
  state->masterEffects->init((float)sampleRate);
  state->masterLimiter->init((float)sampleRate);
  state->masterLimiterActive = 0;
  state->masterEffects->reserve((size_t)state->mixBufferSize / 2);
  delete state->masterInsertChain;
  state->masterInsertChain = nullptr;
  for (auto*& chain : state->insertChains) {
    delete chain;
    chain = nullptr;
  }
  // Reinitialization can change the sample rate; reclaim all old DSP state
  // while audio is stopped before preparing instances at the new rate.
  state->audioCommands->discardProject();
  state->masterInsertChain = new(std::nothrow) InsertChain((float)sampleRate);
  if (state->masterInsertChain)
    state->masterInsertChain->sync(state->audioProject.masterInserts, &state->masterInsertAutomation);
  voiceReclaim(state, true);  // audio is stopped across InitChips
  if (state->sidPreview) state->sidPreview->init((float)sampleRate);
  if (state->simpleChipPreview) state->simpleChipPreview->init((float)sampleRate);
  if (state->fourOpPreview) state->fourOpPreview->init((float)sampleRate);
  if (state->oplPreview) state->oplPreview->init((float)sampleRate);
  if (state->dx7Preview) state->dx7Preview->init((float)sampleRate);
  if (state->opllPreview) state->opllPreview->init((float)sampleRate);
  state->opllPreviewTrack = -1;
  for (int i = 0; i < PROJECT_MAX_TRACKS; i++) {
    state->trackTilt[i].init((float)sampleRate);
    state->insertChains[i] = new(std::nothrow) InsertChain((float)sampleRate);
    if (state->insertChains[i]) state->insertChains[i]->sync(state->audioProject.trackInserts[i], &state->playbackState.tracks[i].inserts);
    state->insertResetSeen[i] = state->playbackState.tracks[i].insertReset;
    // A closed gate means any slot pointers are leftovers of a retired set that
    // voiceReclaim() just deleted; clear them before re-initialising the rest.
#define X(F, T, A) if (!state->A[i][0]) for (int voice = 0; voice < CHORD_MAX_VOICES; ++voice) state->A[i][voice] = nullptr; \
    else for (int voice = 0; voice < CHORD_MAX_VOICES; ++voice) state->A[i][voice]->init((float)sampleRate);
    CN_VOICE_FAMILIES(X)
#undef X
    if (state->dx7Parts[i]) state->dx7Parts[i]->init((float)sampleRate);
  }
  // Lazy modes: build what the song can reach, now that the rate is known.
  voiceEnsure(state, &state->audioProject, false);

  state->audioCommands->initializeBeads(state);

  // Use provided factory or default
  ChipFactory chipFactory = factory ? factory : defaultChipFactory;

  // Initialize chips based on project's chipsCount
  for (int i = 0; i < state->audioProject.chipsCount; i++) {
    state->chips[i] = chipFactory(i, sampleRate, state->audioProject.chipSetup);
  }
}

static int hasAudioRateModulation(const ChipNomadState* state) {
  for (int trackIdx = 0; trackIdx < state->audioProject.tracksCount; ++trackIdx) {
    const PlaybackTrackState* track = &state->playbackState.tracks[trackIdx];
    for (int i = 0; i < 4; ++i) {
      const PlaybackModState* mod = &track->note.modulation[i];
      if (mod->modulation && mod->modulation->type == ModulationType::FLFO &&
          mod->modulation->destination != 0) return 1;
    }
  }
  return 0;
}

int chipnomadReserveRenderBuffers(ChipNomadState* state, int frames) {
  if (!state || frames <= 0 || frames > INT_MAX / 2) return 1;
  int requiredSize = frames * 2;
  if (!state->audioMonitor->reserve(frames)) return 1;
  state->masterEffects->reserve((size_t)frames);
  if (!(requiredSize <= state->mixBufferSize || resizeMixBuffers(state, requiredSize))) return 1;
  return 0;
}

int chipnomadQueueTrackEnabled(ChipNomadState* state, const uint8_t enabled[PROJECT_MAX_TRACKS]) {
  return state && state->audioCommands && enabled ? state->audioCommands->pushTrackEnabled(enabled) : 0;
}

int chipnomadQueueProjectRefresh(ChipNomadState* state) {
  if (!state || !state->audioCommands) return 0;
  // Build voices for the new project (and drop idle ones) BEFORE the audio
  // thread can adopt it, so the callback never sees an instrument whose
  // voices do not exist yet.
  voiceEnsure(state, &state->project, true);
  return state->audioCommands->pushProject(state->project);
}

int chipnomadQueuePlaybackScale(ChipNomadState* state, uint8_t root, ScalePreset preset) {
  return state && state->audioCommands ? state->audioCommands->pushScale(root, preset) : 0;
}

void chipnomadDiscardQueuedProject(ChipNomadState* state) {
  if (state && state->audioCommands) state->audioCommands->discardProject();
}

void chipnomadQueuePlaybackStop(ChipNomadState* state) {
  if (state && state->audioCommands) state->audioCommands->requestStop();
}

int chipnomadQueuePlaybackStartSong(ChipNomadState* state, int songRow, int chainRow, int loop) {
  if (state) voiceEnsure(state, &state->project, false);
  return state && state->audioCommands ? state->audioCommands->pushCommand(0, songRow, chainRow, loop) : 0;
}
int chipnomadQueuePlaybackStartChain(ChipNomadState* state, int trackIdx, int songRow, int chainRow, int loop) {
  if (state) voiceEnsure(state, &state->project, false);
  return state && state->audioCommands ? state->audioCommands->pushCommand(1, trackIdx, songRow, chainRow, loop) : 0;
}
int chipnomadQueuePlaybackStartPhrase(ChipNomadState* state, int trackIdx, int songRow, int chainRow, int loop) {
  if (state) voiceEnsure(state, &state->project, false);
  return state && state->audioCommands ? state->audioCommands->pushCommand(2, trackIdx, songRow, chainRow, loop) : 0;
}
int chipnomadQueuePlaybackStartPhraseRow(ChipNomadState* state, int trackIdx, const PhraseRow* row) {
  if (state && row) voiceEnsureInstrument(state, trackIdx, row->instrument);
  return state && state->audioCommands && row ? state->audioCommands->pushCommand(3, trackIdx, 0, 0, 0, row) : 0;
}
// Full-sample one-shot preview of a LAZY sample: same as StartPhraseRow but
// sets the track's sliceBypass flag so the voice ignores slice mapping and
// the row's note pitch, playing the whole region at the sample's original
// pitch and speed.
int chipnomadQueuePlaybackStartPhraseRowFull(ChipNomadState* state, int trackIdx, const PhraseRow* row) {
  // kStartPhraseRowFull is appended after upstream's preview commands; the
  // numeric value must match the enum (raw IDs 14/15/16 are taken). The enum
  // is private, so the raw literal is used, matching upstream's own pushes.
  if (state && row) voiceEnsureInstrument(state, trackIdx, row->instrument);
  return state && state->audioCommands && row ? state->audioCommands->pushCommand(17, trackIdx, 0, 0, 0, row) : 0;
}
int chipnomadQueuePlaybackQueuePhrase(ChipNomadState* state, int trackIdx, int songRow, int chainRow) {
  if (state) voiceEnsure(state, &state->project, false);
  return state && state->audioCommands ? state->audioCommands->pushCommand(4, trackIdx, songRow, chainRow) : 0;
}
int chipnomadQueuePlaybackStartLiveChain(ChipNomadState* state, int trackIdx, int songRow) {
  if (state) voiceEnsure(state, &state->project, false);
  return state && state->audioCommands ? state->audioCommands->pushCommand(8, trackIdx, songRow) : 0;
}
int chipnomadQueuePlaybackQueueLiveChain(ChipNomadState* state, int trackIdx, int songRow, int urgent) {
  if (state) voiceEnsure(state, &state->project, false);
  return state && state->audioCommands ? state->audioCommands->pushCommand(9, trackIdx, songRow, urgent) : 0;
}
int chipnomadQueuePlaybackPreviewNote(ChipNomadState* state, int trackIdx, uint8_t note, uint8_t instrument) {
  if (state) voiceEnsureInstrument(state, trackIdx, instrument);
  return state && state->audioCommands ? state->audioCommands->pushCommand(5, trackIdx, note, instrument) : 0;
}
int chipnomadQueuePlaybackStopPreview(ChipNomadState* state, int trackIdx) {
  return state && state->audioCommands ? state->audioCommands->pushCommand(6, trackIdx) : 0;
}
int chipnomadQueuePlaybackClearTrackFX(ChipNomadState* state, int trackIdx) {
  return state && state->audioCommands ? state->audioCommands->pushCommand(7, trackIdx) : 0;
}

void chipnomadQueueLoopRange(ChipNomadState* state, LoopRange range) {
  if (state && state->audioCommands) state->audioCommands->pushLoopRange(range);
}
void chipnomadQueueClearLoopRange(ChipNomadState* state) {
  if (state && state->audioCommands) state->audioCommands->clearLoopRange();
}
const PlaybackStatus* chipnomadGetPlaybackStatus(ChipNomadState* state) {
  if (!state) return NULL;
  state->audioCommands->collectBeads();
  voiceServiceMissing(state);  // UI thread: build voices the audio thread asked for
  state->audioCommands->readStatus(&state->uiPlaybackStatus);
  return &state->uiPlaybackStatus;
}
int chipnomadGetCommandOverflow(ChipNomadState* state) {
  return state && state->audioCommands ? state->audioCommands->commandOverflow() : 0;
}
int chipnomadGetRenderBufferOverflow(ChipNomadState* state) {
  return state && state->audioCommands ? state->audioCommands->renderBufferOverflow() : 0;
}
void chipnomadSetRenderBufferOverflow(ChipNomadState* state) {
  if (state && state->audioCommands) state->audioCommands->setRenderBufferOverflow();
}

static void applyVoicePostModulations(const PlaybackTrackState* track, InstrumentType type,
                                      int* attack, int* decay, int* sustain, int* release,
                                      int* shape, int* triggerDecay, int* triggerColor) {
  int* values[] = {attack, decay, sustain, release, shape, triggerDecay, triggerColor};
  for (int i = 0; i < 4; ++i) {
    const PlaybackModState* mod = &track->note.modulation[i];
    if (!mod->modulation) continue;
    int destination = instrumentGenericModDestination(type, mod->modulation->destination);
    if (destination < genericModEnvelopeAttack || destination >= genericModFirstP5) continue;
    int index = destination - genericModEnvelopeAttack;
    int value = playbackModScaleToRange(mod->outValue, 255);
    *values[index] = modulationIsAdditive(mod->modulation->type) ? *values[index] + value : value;
  }
  for (int i = 0; i < 7; ++i) *values[i] = clampInt(*values[i], 0, 255);
}

static void updateAudioRateModulations(ChipNomadState* state) {
  for (int trackIdx = 0; trackIdx < state->audioProject.tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &state->playbackState.tracks[trackIdx];
    for (int i = 0; i < 4; ++i)
      playbackModNextAudio(&track->note.modulation[i], (float)state->sampleRate);
  }
  updateSoundbankVoices(state);
  updateSampleVoices(state);
  updateSCWFVoices(state);
  updateBraidsVoices(state);
  updatePlaitsVoices(state);
  updatePlaitsAltVoices(state);
  updateAChChidVoices(state);
  updateDrumSynthVoices(state);
  updateMMEVoices(state);
  updateSinteredVoices(state); updateOPLLVoices(state); updateOPLVoices(state); updateFourOpVoices(state); updateSimpleChipVoices(state); updateSIDVoices(state); updateDX7Voices(state);
  updateInsertValues(state);
}

static int advancePlaybackFrame(ChipNomadState* state, uint64_t dueMicros) {
  const int wasPlaying = playbackIsPlaying(&state->playbackState);
  state->audioCommands->applyProject(state);
  state->playbackState.p = &state->audioProject;
  if (state->audioCommands->takeStopRequest()) {
    chipnomadMidiPanic(state);
    playbackStop(&state->playbackState);
    killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);killIf(state->opllPreview); killIf(state->oplPreview);killIf(state->simpleChipPreview); state->opllPreviewTrack = -1;
  }
  for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) {
    if (!state->insertChains[t]) continue;
    auto& track = state->playbackState.tracks[t];
    state->insertChains[t]->setTempo(state->audioProject.tickRate);
    uint16_t cleared = state->insertChains[t]->sync(state->audioProject.trackInserts[t], &track.inserts,
        t < state->audioProject.tracksCount && state->playbackState.trackEnabled[t]);
    for (int address = 0; address < 16; ++address) if (cleared & (1u << address)) {
      track.slewTarget[fxF11 + address] = -1;
      track.slewRemaining[fxF11 + address] = 0;
    }
  }
  if (state->masterInsertChain)
    state->masterInsertChain->setTempo(state->audioProject.tickRate);
  if (state->masterInsertChain)
    state->masterInsertChain->sync(state->audioProject.masterInserts, &state->masterInsertAutomation);
  state->audioCommands->applySettings(&state->playbackState);
  state->audioCommands->applyCommands(state);
  if (!wasPlaying && playbackIsPlaying(&state->playbackState)) state->masterEffects->reset();
  for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) {
    auto& track = state->playbackState.tracks[t];
    if (track.insertReset != state->insertResetSeen[t]) {
      if (state->insertChains[t]) state->insertChains[t]->reset();
      state->insertResetSeen[t] = track.insertReset;
    }
  }
  float axes[4];
  for (int i = 0; i < 4; ++i) axes[i] = chipnomadLiveStickAxis(i);
  int enabled = chipnomadLiveStickIsEnabled();
  if (!enabled && chipnomadMotionRateResetPending()) enabled = 1;
  playbackUpdateLiveStickModulation(&state->playbackState, axes, enabled);
  state->frameSampleCounter += state->sampleRate / state->audioProject.tickRate;
  int allTracksStopped = playbackNextFrame(state);
  if (state->voiceMode != ChipNomadVoiceMode::eager) voiceAuditTracks(state);
  if (!wasPlaying && playbackIsPlaying(&state->playbackState) && state->masterInsertChain)
    state->masterInsertChain->reset();
  for (int t = 0; t < state->audioProject.tracksCount; ++t)
    if (state->playbackState.tracks[t].note.noteTriggered && state->insertChains[t])
      state->insertChains[t]->trigger();
  motionRecordFrame(state);
  if (allTracksStopped) playbackUpdateLiveStickModulation(&state->playbackState, axes, enabled);
  updateSoundbankVoices(state);
  updateSampleVoices(state); updateSCWFVoices(state); updateBraidsVoices(state);
  updatePlaitsVoices(state); updatePlaitsAltVoices(state); updateAChChidVoices(state); updateDrumSynthVoices(state); updateMMEVoices(state); updateSinteredVoices(state); updateOPLLVoices(state); updateOPLVoices(state); updateFourOpVoices(state); updateSimpleChipVoices(state); updateSIDVoices(state); updateDX7Voices(state); applyVoiceEvents(state, dueMicros);
  if (state->audioOverload > 0) state->audioOverload--;
  for (int i = 0; i < PROJECT_MAX_TRACKS; ++i)
    if (state->trackClipping[i] > 0) state->trackClipping[i]--;
  updateInsertValues(state);
  detectAYPitchConflicts(state);
  state->audioCommands->publishStatus(&state->playbackState);
  if (playbackIsPlaying(&state->playbackState) && state->opllPreviewTrack >= 0) {
    killIf(state->sidPreview);killIf(state->fourOpPreview);killIf(state->dx7Preview);killIf(state->opllPreview); killIf(state->oplPreview);killIf(state->simpleChipPreview); state->opllPreviewTrack = -1;
  }
  return allTracksStopped && state->opllPreviewTrack < 0 && !state->masterEffects->tailActive();
}

static int prepareRenderChunk(ChipNomadState* state, float* output, int frames) {
  for (int i = 0; i < state->audioProject.tracksCount; ++i) state->voiceMonitors[i].active = 0;
  int requiredSize = frames * 2;
  if (requiredSize > state->mixBufferSize) {
    state->audioCommands->setRenderBufferOverflow();
    return 0;
  }
  for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) {
    state->insertActive[t] = state->insertChains[t] && state->insertChains[t]->active();
    if (state->insertActive[t]) memset(state->insertBuffer + (size_t)t * state->mixBufferSize, 0, requiredSize * sizeof(float));
  }
  state->audioMonitor->beginChunk(frames);
  memset(output, 0, requiredSize * sizeof(float));
  memset(state->reverbBuffer, 0, requiredSize * sizeof(float));
  memset(state->delayBuffer, 0, requiredSize * sizeof(float));
  return 1;
}

static void renderChipTracks(ChipNomadState* state, float* output, int frames) {
  for (int chipIdx = 0; chipIdx < state->audioProject.chipsCount; ++chipIdx) {
    if (chipIdx >= state->audioProject.tracksCount || !state->playbackState.trackEnabled[chipIdx]) continue;
    uint8_t instrumentIdx = state->playbackState.tracks[chipIdx].note.instrument;
    if (instrumentIdx >= PROJECT_MAX_INSTRUMENTS) continue;
    InstrumentType type = state->audioProject.instruments[instrumentIdx].type;
    if (type != InstrumentType::AY1 && type != InstrumentType::AY2 && type != InstrumentType::AYSample) continue;
    SoundChip* chip = state->chips[chipIdx];
    if (!chip) continue;
    float* rendered = state->mixBuffer;
    chip->render(rendered, frames);
    float gain = state->audioProject.trackVolume[chipIdx] / 100.0f * state->audioProject.instruments[instrumentIdx].volume / 255.0f;
    float reverbSend = effectiveTrackSend(state, chipIdx, true);
    float delaySend = effectiveTrackSend(state, chipIdx, false);
    int instrumentPan = effectivePan(state, chipIdx, true);
    int trackPan = effectivePan(state, chipIdx, false);
    TrackPanMix pan = makeTrackPanMix(state, instrumentPan, trackPan);
    for (int i = 0; i < frames; ++i)
      mixTrackFrame(state, chipIdx, output, state->reverbBuffer, state->delayBuffer,
                    rendered[i * 2] * gain, rendered[i * 2 + 1] * gain,
                    i, reverbSend, delaySend, pan);
  }
}

template <typename Voice>
static void renderMonoVoiceTracks(ChipNomadState* state, Voice* const voices[][CHORD_MAX_VOICES], float* output, int frames) {
  for (int trackIdx = 0; trackIdx < state->audioProject.tracksCount; ++trackIdx) {
    if (!state->playbackState.trackEnabled[trackIdx]) continue;
    if (!gateLoad(&voices[trackIdx][0])) continue;  // lazy: set not built yet
    for (int slot = 0; slot < state->playbackState.tracks[trackIdx].chordVoiceCount; ++slot) {
      Voice* voice = voices[trackIdx][slot];
      if (!voice->active()) continue;
      float* rendered = state->mixBuffer;
      voice->render(rendered, frames);
      captureVoiceMonitor(state, trackIdx, rendered, frames, 1, voice->envelopeLevel());
      float trackGain = state->audioProject.trackVolume[trackIdx] / 100.0f;
      float reverbSend = effectiveTrackSend(state, trackIdx, true);
      float delaySend = effectiveTrackSend(state, trackIdx, false);
      int instrumentPan = effectivePan(state, trackIdx, true);
      int trackPan = effectivePan(state, trackIdx, false);
      TrackPanMix pan = makeTrackPanMix(state, instrumentPan, trackPan);
      for (int i = 0; i < frames; ++i) {
        float sample = rendered[i] * 0.25f * trackGain;
        mixTrackFrame(state, trackIdx, output, state->reverbBuffer, state->delayBuffer,
                      sample, sample, i, reverbSend, delaySend, pan);
      }
    }
  }
}

template <typename Voice>
static void renderStereoVoiceTracks(ChipNomadState* state, Voice* const voices[][CHORD_MAX_VOICES], float* output, int frames, bool includeReleaseSlots = false) {
  for (int trackIdx = 0; trackIdx < state->audioProject.tracksCount; ++trackIdx) {
    if (!state->playbackState.trackEnabled[trackIdx]) continue;
    if (!gateLoad(&voices[trackIdx][0])) continue;  // lazy: set not built yet
    for (int slot = 0; slot < (includeReleaseSlots ? CHORD_MAX_VOICES : state->playbackState.tracks[trackIdx].chordVoiceCount); ++slot) {
      Voice* voice = voices[trackIdx][slot];
      if (!voice->active()) continue;
      float* rendered = state->mixBuffer;
      voice->render(rendered, frames);
      captureVoiceMonitor(state, trackIdx, rendered, frames, 2, voice->envelopeLevel());
      float gain = state->audioProject.trackVolume[trackIdx] / 100.0f;
      float reverbSend = effectiveTrackSend(state, trackIdx, true);
      float delaySend = effectiveTrackSend(state, trackIdx, false);
      int instrumentPan = effectivePan(state, trackIdx, true);
      int trackPan = effectivePan(state, trackIdx, false);
      TrackPanMix pan = makeTrackPanMix(state, instrumentPan, trackPan);
      for (int i = 0; i < frames; ++i)
        mixTrackFrame(state, trackIdx, output, state->reverbBuffer, state->delayBuffer,
                      rendered[i * 2] * gain, rendered[i * 2 + 1] * gain,
                      i, reverbSend, delaySend, pan);
    }
  }
}

static void updateInsertValues(ChipNomadState* state) {
  for (int t = 0; t < state->audioProject.tracksCount; ++t) {
    auto& track = state->playbackState.tracks[t];
    for (int slot = 0; slot < 2; ++slot) {
      const auto& c = state->audioProject.trackInserts[t][slot];
      const auto& d = insertDescriptor(c.module);
      for (int p = 0; p < d.count; ++p) {
        int value = track.inserts.valid[slot] & (1 << p) ? track.inserts.values[slot][p] : c.values[p];
        int range = d.parameters[p].mapping == InsertMapping::discrete ? (int)d.parameters[p].maximum : 255;
        if (d.parameters[p].mapping != InsertMapping::discrete) value = slewEngineFX(&track, (FX)(fxF11 + slot * 8 + p), value);
        if (track.note.instrument != EMPTY_VALUE_8) {
          auto type = state->audioProject.instruments[track.note.instrument].type;
          for (auto& mod : track.note.modulation) {
            if (!mod.modulation || instrumentGenericModDestination(type, mod.modulation->destination) != genericModFirstInsert + slot * 8 + p) continue;
            int offset = playbackModScaleToRange(mod.outValue, range);
            value = modulationIsAdditive(mod.modulation->type) || mod.modulation->type == ModulationType::SLFO || mod.modulation->type == ModulationType::FLFO ? value + offset : offset;
          }
        }
        state->insertValues[t][slot][p] = insertClamp(c.module, p, value);
      }
    }
  }
}

static void processTrackInserts(ChipNomadState* state, float* output, int frames) {
  for (int t = 0; t < state->audioProject.tracksCount; ++t) {
    if (!state->insertActive[t] || !state->playbackState.trackEnabled[t]) continue;
    float* samples = state->insertBuffer + (size_t)t * state->mixBufferSize;
    state->insertChains[t]->process(samples, frames, state->insertValues[t]);
    float reverb = effectiveTrackSend(state, t, true), delay = effectiveTrackSend(state, t, false);
    for (int i = 0; i < frames * 2; ++i) {
      float sample = samples[i], previous = output[i];
      state->audioMonitor->add(t, i, sample);
      output[i] += sample;
      if (reverb != 0.0f) state->reverbBuffer[i] += sample * reverb;
      if (delay != 0.0f) state->delayBuffer[i] += sample * delay;
      if (fabsf(output[i]) > 1.0f && fabsf(output[i]) > fabsf(previous)) state->trackClipping[t] = AUDIO_OVERLOAD_COOLDOWN_FRAMES;
    }
  }
}

static void processMasterMix(ChipNomadState* state, float* output, int frames) {
  bool hasReverb = false, hasDelay = false;
  for (int i = 0; i < state->audioProject.tracksCount; ++i) {
    hasReverb |= effectiveTrackSend(state, i, true) > 0.0f || state->audioProject.delayReverbSend > 0;
    hasDelay |= effectiveTrackSend(state, i, false) > 0.0f;
  }
  state->masterEffects->process(hasReverb ? state->reverbBuffer : NULL,
                                hasDelay ? state->delayBuffer : NULL, output, frames, &state->audioProject);
  if (state->masterInsertChain && state->masterInsertChain->active()) {
    uint8_t values[2][8];
    for (int slot = 0; slot < 2; ++slot) memcpy(values[slot], state->audioProject.masterInserts[slot].values, 8);
    state->masterInsertChain->process(output, frames, values);
  }
  for (int i = 0; i < frames * 2; ++i) {
    output[i] *= state->mixVolume;
    // audioOverload: "this bus WOULD have clipped" - measured before the
    // limiter, so it means the same with the limiter on or off.
    if (output[i] > 1.0f || output[i] < -1.0f) state->audioOverload = AUDIO_OVERLOAD_COOLDOWN_FRAMES;
  }
  // Master limiter: last stage on the bus, in place, no allocation. The
  // off->on edge is handled here (audio thread) so the UI thread never touches
  // limiter state.
  const int limiterOn = __atomic_load_n(&state->masterLimiterEnabled, __ATOMIC_RELAXED);
  if (limiterOn) {
    if (!state->masterLimiterActive) state->masterLimiter->reset();
    state->masterLimiterActive = 1;
    state->masterLimiter->process(output, frames);
  } else {
    state->masterLimiterActive = 0;
  }
}

int chipnomadDefaultMasterLimiter(void) {
#ifdef RGNANO_BUILD
  return 1;
#else
  return 0;
#endif
}

void chipnomadSetMasterLimiter(ChipNomadState* state, int enabled) {
  if (state) __atomic_store_n(&state->masterLimiterEnabled, enabled ? 1 : 0, __ATOMIC_RELAXED);
}

int chipnomadTakeLimiterStats(ChipNomadState* state, float* minGain, float* meanReduction, uint32_t* engagedFrames, uint32_t* totalFrames) {
  if (!state || !state->masterLimiter) return 0;
  MasterLimiter::Stats s = state->masterLimiter->takeStats();
  if (minGain) *minGain = s.minGain;
  if (meanReduction) *meanReduction = s.meanReduction;
  if (engagedFrames) *engagedFrames = s.engagedFrames;
  if (totalFrames) *totalFrames = s.totalFrames;
  return 1;
}

static int chipnomadRenderImpl(ChipNomadState* state, float* buffer, int samples);

// renderSeq is odd while a render call runs; voiceReclaim() uses it to know
// when no render call can still hold a pointer to a retired voice set.
int chipnomadRender(ChipNomadState* state, float* buffer, int samples) {
  if (!state) return 0;
  __atomic_add_fetch(&state->renderSeq, 1u, __ATOMIC_SEQ_CST);
  int rendered = chipnomadRenderImpl(state, buffer, samples);
  __atomic_add_fetch(&state->renderSeq, 1u, __ATOMIC_SEQ_CST);
  return rendered;
}

static int chipnomadRenderImpl(ChipNomadState* state, float* buffer, int samples) {
  if (!state || !buffer || samples <= 0 || samples > INT_MAX / 2) return 0;
  // Real wall-clock reference for this callback: a row that lands N samples
  // into it is due N/sampleRate seconds after "now", not "now" itself - see
  // midiRouterEmitNoteOn for why this matters (this callback can compute
  // several rows' worth of MIDI events well ahead of when they actually
  // play).
  uint64_t callbackStartMicros = midiRouterNowMicros();
  state->audioMonitor->beginRender();
  int samplesLeft = samples;
  while (samplesLeft > 0) {
    uint64_t dueMicros = callbackStartMicros +
      (uint64_t)((double)(samples - samplesLeft) * 1000000.0 / state->sampleRate);
    if ((int)state->frameSampleCounter == 0 && advancePlaybackFrame(state, dueMicros)) break;
    int frames = (int)state->frameSampleCounter < samplesLeft ? (int)state->frameSampleCounter : samplesLeft;
    if (hasAudioRateModulation(state)) { updateAudioRateModulations(state); frames = 1; }
    float* output = buffer + (samples - samplesLeft) * 2;
    if (!prepareRenderChunk(state, output, frames)) return 0;
    renderChipTracks(state, output, frames);
    renderMonoVoiceTracks(state, state->braidsVoices, output, frames);
    // Removed chord slots may still have a short de-click tail after KILL.
    renderStereoVoiceTracks(state, state->soundbankVoices, output, frames, true);
    renderStereoVoiceTracks(state, state->sampleVoices, output, frames);
    renderStereoVoiceTracks(state, state->scwfVoices, output, frames);
    renderMonoVoiceTracks(state, state->plaitsVoices, output, frames);
    renderMonoVoiceTracks(state, state->plaitsAltVoices, output, frames);
    renderMonoVoiceTracks(state, state->achchidVoices, output, frames);
    renderMonoVoiceTracks(state, state->drumSynthVoices, output, frames);
    renderMonoVoiceTracks(state, state->mmeVoices, output, frames);
    renderMonoVoiceTracks(state, state->sinteredVoices, output, frames);
    renderMonoVoiceTracks(state, state->opllVoices, output, frames);
    for(int t=0;t<state->audioProject.tracksCount;++t) {
      auto* part=gateLoad(&state->dx7Parts[t]);
      if(!part)continue;
      int instrument=state->playbackState.tracks[t].note.instrument;
      bool dx7Selected=instrument!=EMPTY_VALUE_8&&state->audioProject.instruments[instrument].type==InstrumentType::DX7;
      if(!part->active()&&!dx7Selected)continue;
      float* rendered = state->mixBuffer;
      part->render(rendered,frames);
      if(!state->playbackState.trackEnabled[t])continue;
      captureVoiceMonitor(state,t,rendered,frames,1,part->envelopeLevel());
      float gain=state->audioProject.trackVolume[t]/100.f*.25f;
      float reverb=effectiveTrackSend(state,t,true),delay=effectiveTrackSend(state,t,false);
      int instrumentPan = effectivePan(state, t, true);
      int trackPan = effectivePan(state, t, false);
      TrackPanMix pan = makeTrackPanMix(state, instrumentPan, trackPan);
      for(int i=0;i<frames;++i)
        mixTrackFrame(state,t,output,state->reverbBuffer,state->delayBuffer,rendered[i]*gain,rendered[i]*gain,i,reverb,delay,pan);
    }

    renderStereoVoiceTracks(state,state->fourOpVoices,output,frames);
    renderStereoVoiceTracks(state,state->oplVoices,output,frames);
    renderMonoVoiceTracks(state,state->simpleChipVoices,output,frames);
    SIDVoice* sidPool[PROJECT_MAX_TRACKS*CHORD_MAX_VOICES];int sidCount=0;
    // Order roots first across tracks; deterministic stealing retains new roots.
    for(int slot=0;slot<CHORD_MAX_VOICES;++slot)for(int t=0;t<PROJECT_MAX_TRACKS;++t)if(gateLoad(&state->sidVoices[t][0]))sidPool[sidCount++]=state->sidVoices[t][slot];
    limitSIDVoices(sidPool,sidCount,4);
    renderMonoVoiceTracks(state,state->sidVoices,output,frames);
    if (state->opllPreviewTrack >= 0 && state->playbackState.trackEnabled[state->opllPreviewTrack]) {
      const int track = state->opllPreviewTrack;
      bool opl = isOPL(state->chipPreviewType)||isFourOp(state->chipPreviewType);
      if(state->chipPreviewType==InstrumentType::SID)state->sidPreview->render(state->mixBuffer,frames);else if(state->chipPreviewType==InstrumentType::DX7)state->dx7Preview->render(state->mixBuffer,frames);else if(isFourOp(state->chipPreviewType))state->fourOpPreview->render(state->mixBuffer,frames);else if (opl) state->oplPreview->render(state->mixBuffer,frames);else if(isSimpleChip(state->chipPreviewType))state->simpleChipPreview->render(state->mixBuffer,frames);else state->opllPreview->render(state->mixBuffer, frames);
      float gain = state->audioProject.trackVolume[track] / 100.0f * (opl ? 1.0f : 0.25f);
      float reverbSend = effectiveTrackSend(state, track, true);
      float delaySend = effectiveTrackSend(state, track, false);
      int instrumentPan = effectivePan(state, track, true);
      int trackPan = effectivePan(state, track, false);
      TrackPanMix pan = makeTrackPanMix(state, instrumentPan, trackPan);
      for (int i = 0; i < frames; ++i) {
        float left = state->mixBuffer[opl ? i * 2 : i] * gain;
        float right = state->mixBuffer[opl ? i * 2 + 1 : i] * gain;
        mixTrackFrame(state, track, output, state->reverbBuffer, state->delayBuffer, left, right, i,
                      reverbSend, delaySend, pan);
      }
    }
    processTrackInserts(state, output, frames);
    processMasterMix(state, output, frames);
    state->audioMonitor->finishChunk(output, frames, state->sampleRate);
    samplesLeft -= frames;
    state->frameSampleCounter -= (float)frames;
  }
  if (samplesLeft > 0) memset(buffer + (samples - samplesLeft) * 2, 0, samplesLeft * 2 * sizeof(float));
  if (samplesLeft > 0) {
    state->audioMonitor->beginChunk(samplesLeft);
    state->audioMonitor->finishChunk(buffer + (samples - samplesLeft) * 2, samplesLeft, state->sampleRate);
  }
  state->audioMonitor->publish();
  return samples - samplesLeft;
}

static float envelopeTime(uint8_t value) {
  float normalized = value / 255.0f;
  return normalized * normalized * 5.0f;
}

static void applyVoiceEvents(ChipNomadState* state, uint64_t dueMicros) {
  PlaybackState* playback = &state->playbackState;
  Project* project = &state->audioProject;
  for (int trackIdx = 0; trackIdx < project->tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];

    // MC1-MC4 row FX (see playback_fx_midi.cpp): independent of note
    // trigger/release, so this runs even on a row that only carries a CC
    // change. Only meaningful for a MIDI Out instrument; the FX is silently
    // inert (already recorded as pending, just dropped here) on any other
    // instrument type since a raw CC number has no equivalent there.
    for (int slot = 0; slot < 4; ++slot) {
      if (!track->midiCCPending[slot]) continue;
      track->midiCCPending[slot] = 0;
      if (track->note.instrument == EMPTY_VALUE_8) continue;
      Instrument* instrument = &project->instruments[track->note.instrument];
      if (instrument->type != InstrumentType::Midi) continue;
      uint8_t ccNumber = instrument->chip.midi.ccNumber[slot];
      if (ccNumber == EMPTY_VALUE_8) continue;
      uint8_t channel = instrument->chip.midi.channel & 0x0f;
      uint8_t value = (uint8_t)(track->midiCCValue[slot] * 127 / 255);
      midiRouterEmitCC(state->midiRouter, channel, ccNumber, value, dueMicros);
    }

    if (!track->note.noteTriggered && !track->note.noteReleased && !track->note.noteKilled) continue;
    if (track->note.instrument == EMPTY_VALUE_8) continue;
    {
      // Lazy voices: if this instrument's set is not built (realtime mode, UI
      // has not caught up) drop the event rather than touch a NULL set.
      int family = voiceFamilyForType(project->instruments[track->note.instrument].type);
      if (family >= 0 && !voiceFamilyReady(state, trackIdx, family)) {
        voiceFlagMissing(state, trackIdx, family);
        track->note.noteTriggered = track->note.noteReleased = track->note.noteKilled = 0;
        continue;
      }
    }
    auto applyEvent = [&](auto* voices) {
      for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) {
        if (track->note.noteKilled || (track->note.noteTriggered && slot >= track->chordVoiceCount))
          voices[slot]->kill();
        else if (track->note.noteTriggered) voices[slot]->noteOn();
        else voices[slot]->noteOff();
      }
    };
    switch (project->instruments[track->note.instrument].type) {
      case InstrumentType::SoundFont:
        if (track->note.noteTriggered)
          for (int incoming = 0; incoming < track->chordVoiceCount; ++incoming)
            for (int t = 0; t < project->tracksCount; ++t)
              for (int v = 0; v < CHORD_MAX_VOICES && gateLoad(&state->soundbankVoices[t][0]); ++v)
                state->soundbankVoices[t][v]->chokeFrom(
                    *state->soundbankVoices[trackIdx][incoming]);
        applyEvent(state->soundbankVoices[trackIdx]);
        break;
      case InstrumentType::Sample:
        applyEvent(state->sampleVoices[trackIdx]);
        break;
      case InstrumentType::SCWF:
      case InstrumentType::BYOWTBL:
        applyEvent(state->scwfVoices[trackIdx]);
        break;
      case InstrumentType::Braids:
        applyEvent(state->braidsVoices[trackIdx]);
        break;
      case InstrumentType::Plaits:
        applyEvent(state->plaitsVoices[trackIdx]);
        break;
      case InstrumentType::PlaitsAlt:
        applyEvent(state->plaitsAltVoices[trackIdx]);
        break;
      case InstrumentType::AChChid:
        for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) {
          AChChidVoice* voice = state->achchidVoices[trackIdx][slot];
          if (track->note.noteKilled || (track->note.noteTriggered && slot >= track->chordVoiceCount)) voice->kill();
          else if (track->note.noteTriggered) {
            PlaybackFXState* slide = &track->note.fx[fxASL];
            voice->noteOn(track->chordPitchFinal[slot], track->note.accent != 0, slide->isOn != 0, slide->fxValue);
          } else voice->noteOff();
        }
        break;
      case InstrumentType::DrumSynth:
        for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot)
          if (track->note.noteKilled || (track->note.noteTriggered && slot >= track->chordVoiceCount)) state->drumSynthVoices[trackIdx][slot]->kill();
          else if (track->note.noteTriggered) state->drumSynthVoices[trackIdx][slot]->noteOn();
        break;
      case InstrumentType::SID:
        applyEvent(state->sidVoices[trackIdx]);break;
      case InstrumentType::SegaPSG:
      case InstrumentType::GBPulse:
      case InstrumentType::GBNoise:
        applyEvent(state->simpleChipVoices[trackIdx]);break;
      case InstrumentType::GenesisFM:
      case InstrumentType::ArcadeFM:
        applyEvent(state->fourOpVoices[trackIdx]);break;
      case InstrumentType::OPL2:
      case InstrumentType::OPL3:
        applyEvent(state->oplVoices[trackIdx]);break;
      case InstrumentType::DX7:
        applyEvent(state->dx7Voices[trackIdx]);break;
      case InstrumentType::OPLL:
      case InstrumentType::VRC7:
        applyEvent(state->opllVoices[trackIdx]);
        break;
      case InstrumentType::MME:
        applyEvent(state->mmeVoices[trackIdx]);
        break;
      case InstrumentType::Sintered:
        for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot)
          if (track->note.noteKilled || (track->note.noteTriggered && slot >= track->chordVoiceCount)) state->sinteredVoices[trackIdx][slot]->kill();
          else if (track->note.noteTriggered) state->sinteredVoices[trackIdx][slot]->noteOn();
        break;
      case InstrumentType::Midi: {
        // No voice object: send real MIDI Note On/Off instead, through the
        // router (see midi/midi_router.h), which tracks the active note per
        // slot - so a pitch slide between trigger and release can't turn it
        // into a stuck note - and the Program/Bank "already sent" cache.
        InstrumentMidi* midiParams = &project->instruments[track->note.instrument].chip.midi;
        uint8_t channel = midiParams->channel & 0x0f;
        if (track->note.noteTriggered) {
          midiRouterEmitProgramBank(state->midiRouter, channel, midiParams->program, midiParams->bankHigh, midiParams->bankLow, dueMicros);
        }
        for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) {
          int endSlot = track->note.noteKilled || track->note.noteReleased ||
                        (track->note.noteTriggered && slot >= track->chordVoiceCount);
          // midiRouterEmitNoteOff/On are no-ops (Off) or release-then-send
          // (On, if this slot was still active) on their own, matching the
          // *active-gated sends this replaced.
          if (endSlot) midiRouterEmitNoteOff(state->midiRouter, trackIdx, slot, dueMicros);
          if (track->note.noteTriggered && slot < track->chordVoiceCount) {
            int midiNote = 12 + track->chordPitchFinal[slot];
            if (midiNote < 0) midiNote = 0;
            if (midiNote > 127) midiNote = 127;
            int volume = clampInt(track->note.volume + track->note.volumeOffset, 0, PHRASE_VOLUME_MAX);
            int velocity = volume;
            midiRouterEmitNoteOn(state->midiRouter, trackIdx, slot, channel, (uint8_t)midiNote, (uint8_t)velocity, dueMicros);
          }
        }
        break;
      }
      default: break;
    }
    track->note.noteTriggered = track->note.noteReleased = track->note.noteKilled = 0;
  }
  DX7Part* dx7Live[PROJECT_MAX_TRACKS];
  for (int t = 0; t < PROJECT_MAX_TRACKS; ++t) dx7Live[t] = gateLoad(&state->dx7Parts[t]);
  limitDX7Voices(dx7Live, project->tracksCount);
}

// InstrumentType::Midi keeps no voice object of its own (see
// applyVoiceEvents above), so unlike every other instrument type it can't
// naturally decay through its own release stage: an active note left
// without an explicit Note Off stays stuck on the external device. The
// router tracks "still sounding" independently of PlaybackTrackState, so it
// survives a hard track reset (e.g. Stop) that clears noteTriggered/
// noteReleased before applyVoiceEvents ever sees them - see
// midi/midi_router.h's midiRouterPanic for the actual sweep.
void chipnomadMidiPanic(ChipNomadState* state) {
  if (!state) return;
  midiRouterPanic(state->midiRouter);
}

void chipnomadCollectSoundbanks(ChipNomadState* state) {
  if (!state) return;
  soundbank::collect([&](uint32_t h) {
    for (const Project* p : {&state->project, &state->audioProject})
      for (const auto& i : p->instruments)
        if (i.type == InstrumentType::SoundFont && i.chip.soundfont.prepared == h) return true;
    if (state->audioCommands->hasSoundbank(h)) return true;
    for (int t = 0; t < PROJECT_MAX_TRACKS; ++t)
      for (int v = 0; v < CHORD_MAX_VOICES; ++v)
        if (state->soundbankVoices[t][0] && state->soundbankVoices[t][v]->active() && state->soundbankVoices[t][v]->handle() == h)
          return true;
    return false;
  });
}
static void updateSoundbankVoices(ChipNomadState* state) {
  auto* project = &state->audioProject;
  auto* playback = &state->playbackState;
  for (int t = 0; t < project->tracksCount; ++t) {
    auto* track = &playback->tracks[t];
    auto voices = state->soundbankVoices[t];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built
    if (track->note.instrument == EMPTY_VALUE_8 ||
        project->instruments[track->note.instrument].type != InstrumentType::SoundFont) {
      for (int v = 0; v < CHORD_MAX_VOICES; ++v) voices[v]->kill();
      continue;
    }
    auto* instrument = &project->instruments[track->note.instrument];
    const auto controls = soundbankFXControls(instrument->chip.soundfont.controls, track->note.fx);
    float gain = (track->note.fx[fxFVL].isOn ? track->note.fx[fxFVL].fxValue : instrument->volume)
      * playbackVolumeGain(playback, track) / 255.f;
    int pitch = 0;
    for (int m = 0; m < 4; ++m) {
      auto& mod = track->note.modulation[m];
      if (!mod.modulation) continue;
      if (mod.modulation->destination == 1) {
        auto value = playbackModScaleToRange(mod.outValue, 255);
        gain = modulationIsAdditive(mod.modulation->type) ? gain + value / 255.f : value / 255.f;
      } else if (mod.modulation->destination == 2)
        pitch += playbackModScaleToRange(mod.outValue, 1200);
    }
    for (int v = 0; v < track->chordVoiceCount; ++v) {
      int note = track->chordPitchFinal[v];
      int cents = note == EMPTY_VALUE_8 ? 6000
                                        : (project->linearPitch ? project->pitchTable.values[note]
                                                                : (note + 12) * 100) +
                                              track->note.fineOffset + pitch;
      int velocity = std::clamp(int(track->note.volume) * 127 / PHRASE_VOLUME_MAX, 0, 127);
      voices[v]->configure(instrument->chip.soundfont.prepared, cents,
                           gain / std::max(1, int(track->chordVoiceCount)), velocity, 20000,
                           controls);
    }
  }
}

static void updateSampleVoices(ChipNomadState* state) {
  Project* project = &state->audioProject;
  PlaybackState* playback = &state->playbackState;
  for (int trackIdx = 0; trackIdx < project->tracksCount; trackIdx++) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];
    SampleVoice** voices = state->sampleVoices[trackIdx];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built
    if (track->note.instrument == EMPTY_VALUE_8 ||
        project->instruments[track->note.instrument].type != InstrumentType::Sample) {
      for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) voices[slot]->kill();
      continue;
    }

    InstrumentSample* sample = &project->instruments[track->note.instrument].chip.sample;
    int pitchCents = sample->pitch * 100 + track->note.fineOffset;
    int speedPercent = sample->speedPercent;
    int loopMode = sample->loopMode;
    uint8_t start = sample->start;
    uint8_t end = sample->end;
    // D3 (updated): LAZY slices now map chromatically like EQUAL/AUTO. The
    // only exception is the one-shot full-sample preview (kStartPhraseRowFull
    // sets sliceBypass), which plays the whole region at the sample's
    // original pitch - the row's note is ignored entirely.
    uint8_t sliceCount = track->sliceBypass ? 0
      : (sampleActsAsSliced(sample) ? sampleDecodeSliceCount(sample->slice) : 0);
    uint8_t sliceIndex = 0;
    int cutoff = sample->filterCutoffHz;
    int resonance = sample->filterResonance;
    int attack = sample->attack, decay = sample->decay, sustain = sample->sustain;
    int release = sample->release, shape = sample->envelopeShape;
    int triggerDecay = decay, triggerColor = sustain;
    if (sliceCount) {
      uint8_t pitch = track->chordPitchFinal[0] != EMPTY_VALUE_8 ? track->chordPitchFinal[0] : track->note.pitchFinal;
      if (pitch != EMPTY_VALUE_8) {
        // Notes map chromatically from C-0 and wrap around: note N selects
        // slice N % count, so runs past the last slice cycle back to the
        // first one instead of sticking on the last slice.
        sliceIndex = pitch % sliceCount;
      }
    } else if (track->sliceBypass) {
      // Full-sample preview: no note transposition - the sample plays at
      // its original pitch regardless of the row's note.
    } else if (track->chordPitchFinal[0] != EMPTY_VALUE_8) {
      int rootNote = project->pitchTable.octaveSize * 4;
      if (rootNote >= project->pitchTable.length) rootNote = 0;
      int noteCents = project->linearPitch
        ? project->pitchTable.values[track->chordPitchFinal[0]]
        : track->chordPitchFinal[0] * 100;
      int rootCents = project->linearPitch
        ? project->pitchTable.values[rootNote]
        : rootNote * 100;
      pitchCents += noteCents - rootCents;
    }
    float gain = phraseGain(playback, track, &project->instruments[track->note.instrument]);
    if (track->note.fx[fxSPT].isOn) pitchCents = (int8_t)track->note.fx[fxSPT].fxValue * 100 + track->note.fineOffset;
    if (track->note.fx[fxSST].isOn) start = track->note.fx[fxSST].fxValue;
    if (track->note.fx[fxSEN].isOn) end = track->note.fx[fxSEN].fxValue;
    if (track->note.fx[fxSVL].isOn) gain = track->note.fx[fxSVL].fxValue * track->note.volume / (255.0f * PHRASE_VOLUME_MAX);
    if (track->note.fx[fxSCF].isOn) cutoff = instrumentFXCutoff(track->note.fx[fxSCF].fxValue);
    if (track->note.fx[fxSRS].isOn) resonance = track->note.fx[fxSRS].fxValue;
    if (track->note.fx[fxSSP].isOn) speedPercent = track->note.fx[fxSSP].fxValue * 500 / 255;
    // SPL playback modes: 00 forward, 01 reverse, 02 loop, 03 ping-pong.
    uint8_t forceReverse = 0;
    if (track->note.fx[fxSLP].isOn) {
      uint8_t playbackMode = track->note.fx[fxSLP].fxValue;
      forceReverse = playbackMode == 1 ? 1 : 0;
      loopMode = playbackMode == 3 ? 2 : (playbackMode == 2 ? 1 : 0);
    }
    // SLI plays the numbered slice regardless of the note pitch (sliced
    // instruments only; 00 keeps the normal note mapping).
    int sliOverride = track->note.fx[fxSLI].isOn && sliceCount &&
      track->note.fx[fxSLI].fxValue >= 1;
    uint8_t sliSlice = sliOverride
      ? (uint8_t)((track->note.fx[fxSLI].fxValue - 1) % sliceCount) : 0;
    if (sliOverride) sliceIndex = sliSlice;
    for (int i = 0; i < 4; i++) {
      PlaybackModState* mod = &track->note.modulation[i];
      if (!mod->modulation) continue;
      int value = playbackModScaleToRange(mod->outValue, 255);
      switch (mod->modulation->destination) {
        case 1:
          gain *= value / 255.0f;
          break;
        case 2:
          pitchCents += playbackModScaleToRange(mod->outValue, 1200);
          break;
        case 3:
          start = clampInt(start + value, 0, 255);
          break;
        case 4:
          end = clampInt(end + value, 0, 255);
          break;
        case 5:
          speedPercent += playbackModScaleToRange(mod->outValue, 500);
          break;
        case 6:
          loopMode += playbackModScaleToRange(mod->outValue, 2);
          break;
        case 7:
          cutoff = playbackModulateCutoff(cutoff, mod);
          break;
        case 8:
          resonance += value;
          break;
      }
    }
    speedPercent = clampInt(speedPercent, 0, 500);
    loopMode = clampInt(loopMode, 0, 2);
    start = clampInt(start, 0, 255);
    end = clampInt(end, 0, 255);
    cutoff = clampInt(cutoff, FILTER_CUTOFF_MIN_HZ, FILTER_CUTOFF_MAX_HZ);
    resonance = clampInt(resonance, 0, 255);
    if (track->note.fx[fxEAT].isOn) attack = track->note.fx[fxEAT].fxValue;
    if (track->note.fx[fxEDC].isOn) decay = track->note.fx[fxEDC].fxValue;
    if (track->note.fx[fxESU].isOn) sustain = track->note.fx[fxESU].fxValue;
    if (track->note.fx[fxERL].isOn) release = track->note.fx[fxERL].fxValue;
    if (track->note.fx[fxESH].isOn) shape = track->note.fx[fxESH].fxValue;
    applyVoicePostModulations(track, InstrumentType::Sample, &attack, &decay, &sustain, &release,
                              &shape, &triggerDecay, &triggerColor);
    for (int slot = 0; slot < track->chordVoiceCount; ++slot) {
      int voicePitchCents = pitchCents;
      uint8_t voiceSliceIndex = sliceIndex;
      if (sliceCount) {
        uint8_t pitch = track->chordPitchFinal[slot] != EMPTY_VALUE_8 ? track->chordPitchFinal[slot] : track->note.pitchFinal;
        if (pitch != EMPTY_VALUE_8) {
          // Same wrap-around mapping as the main slice index above.
          voiceSliceIndex = pitch % sliceCount;
        }
        if (sliOverride) voiceSliceIndex = sliSlice;
      } else if (track->chordPitchFinal[slot] != EMPTY_VALUE_8) {
        int noteCents = project->linearPitch ? project->pitchTable.values[track->chordPitchFinal[slot]]
          : track->chordPitchFinal[slot] * 100;
        int rootCents = project->linearPitch ? project->pitchTable.values[track->chordPitchFinal[0]]
          : track->chordPitchFinal[0] * 100;
        // A sliced sample keeps its root slice; CRD still supplies voice intervals.
        voicePitchCents += noteCents - rootCents;
      }
      voices[slot]->configure(sample, (float)voicePitchCents, gain / track->chordVoiceCount, (float)speedPercent, start, end, (uint8_t)loopMode,
                              (uint16_t)cutoff, (uint8_t)resonance, attack, decay, sustain, release, shape,
                              sliceCount, voiceSliceIndex, sample->stretchMode, project->tickRate,
                              sample->speedAlgorithm, forceReverse);
    }
  }
}

static void updateSCWFVoices(ChipNomadState* state) {
  Project* project = &state->audioProject;
  PlaybackState* playback = &state->playbackState;
  for (int trackIdx = 0; trackIdx < project->tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];
    SCWFVoice** voices = state->scwfVoices[trackIdx];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built
    if (track->note.instrument == EMPTY_VALUE_8 ||
        (project->instruments[track->note.instrument].type != InstrumentType::SCWF &&
         project->instruments[track->note.instrument].type != InstrumentType::BYOWTBL)) {
      for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) voices[slot]->kill();
      continue;
    }
    Instrument* instrument = &project->instruments[track->note.instrument];
    InstrumentSCWF* scwf = &instrument->chip.scwf;
    InstrumentBYOWTBL* byowtbl = &instrument->chip.byowtbl;
    int detune = scwf->detune;
    int mix = scwf->mix;
    int cutoff = scwf->filterCutoffHz;
    int resonance = scwf->filterResonance;
    int attack = scwf->attack, decay = scwf->decay, sustain = scwf->sustain;
    int release = scwf->release, shape = scwf->envelopeShape;
    int triggerDecay = decay, triggerColor = sustain;
    uint8_t frameIndex[2] = {byowtbl->frameIndex[0], byowtbl->frameIndex[1]};
    int pitchModulation = 0;
    float gain = phraseGain(playback, track, instrument);
    detune = slewEngineFX(track, fxSDT,
      track->note.fx[fxSDT].isOn ? track->note.fx[fxSDT].fxValue : detune);
    mix = slewEngineFX(track, fxSMX,
      track->note.fx[fxSMX].isOn ? track->note.fx[fxSMX].fxValue : mix);
    cutoff = instrumentFXCutoff(slewEngineFX(track, fxSCF2,
      track->note.fx[fxSCF2].isOn ? track->note.fx[fxSCF2].fxValue : filterControlFromCutoff(cutoff)));
    resonance = slewEngineFX(track, fxSRS2,
      track->note.fx[fxSRS2].isOn ? track->note.fx[fxSRS2].fxValue : resonance);
    if (instrument->type == InstrumentType::BYOWTBL) {
      frameIndex[0] = (uint8_t)slewEngineFX(track, fxBIA,
        track->note.fx[fxBIA].isOn ? track->note.fx[fxBIA].fxValue : frameIndex[0]);
      frameIndex[1] = (uint8_t)slewEngineFX(track, fxBIB,
        track->note.fx[fxBIB].isOn ? track->note.fx[fxBIB].fxValue : frameIndex[1]);
    }
    for (int i = 0; i < 4; ++i) {
      PlaybackModState* mod = &track->note.modulation[i];
      if (!mod->modulation) continue;
      int value = playbackModScaleToRange(mod->outValue, 255);
      switch (mod->modulation->destination) {
        case 1: gain = modulationIsAdditive(mod->modulation->type) ? gain + value / 255.0f : value * track->note.volume / (255.0f * PHRASE_VOLUME_MAX); break;
        case 2: pitchModulation += playbackModScaleToRange(mod->outValue, 1200); break;
        case 3: detune += value; break;
        case 4: mix += value; break;
        case 5:
          if (instrument->type == InstrumentType::BYOWTBL) frameIndex[0] = (uint8_t)clampInt(frameIndex[0] + value, 0, 255);
          else cutoff = playbackModulateCutoff(cutoff, mod);
          break;
        case 6:
          if (instrument->type == InstrumentType::BYOWTBL) frameIndex[1] = (uint8_t)clampInt(frameIndex[1] + value, 0, 255);
          else resonance += value;
          break;
        case 7: cutoff = playbackModulateCutoff(cutoff, mod); break;
        case 8: resonance += value; break;
      }
    }
    detune = clampInt(detune, 0, SCWF_DETUNE_MAX);
    mix = clampInt(mix, 0, 255);
    cutoff = clampInt(cutoff, FILTER_CUTOFF_MIN_HZ, FILTER_CUTOFF_MAX_HZ);
    resonance = clampInt(resonance, 0, 255);
    if (track->note.fx[fxEAT].isOn) attack = track->note.fx[fxEAT].fxValue;
    if (track->note.fx[fxEDC].isOn) decay = track->note.fx[fxEDC].fxValue;
    if (track->note.fx[fxESU].isOn) sustain = track->note.fx[fxESU].fxValue;
    if (track->note.fx[fxERL].isOn) release = track->note.fx[fxERL].fxValue;
    if (track->note.fx[fxESH].isOn) shape = track->note.fx[fxESH].fxValue;
    applyVoicePostModulations(track, instrument->type, &attack, &decay, &sustain, &release,
                              &shape, &triggerDecay, &triggerColor);
    for (int slot = 0; slot < track->chordVoiceCount; ++slot) {
      uint8_t pitch = track->chordPitchFinal[slot];
      int cents = pitch == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[pitch] : (pitch + 12) * 100) +
        track->note.fineOffset + pitchModulation;
      voices[slot]->configure(scwf, (float)cents, gain / track->chordVoiceCount, scwfDetuneCents((uint8_t)detune), (uint8_t)mix,
                              (uint16_t)cutoff, (uint8_t)resonance,
                              instrument->type == InstrumentType::BYOWTBL ? byowtbl->frameSize : NULL,
                              instrument->type == InstrumentType::BYOWTBL ? frameIndex : NULL,
                              attack, decay, sustain, release, shape);
    }
  }
}

static void updateAChChidVoices(ChipNomadState* state) {
  Project* project = &state->audioProject;
  PlaybackState* playback = &state->playbackState;
  for (int trackIdx = 0; trackIdx < project->tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];
    AChChidVoice** voices = state->achchidVoices[trackIdx];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built
    if (track->note.instrument == EMPTY_VALUE_8 ||
        project->instruments[track->note.instrument].type != InstrumentType::AChChid) {
      for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) voices[slot]->kill();
      continue;
    }
    InstrumentAChChid* a = &project->instruments[track->note.instrument].chip.achchid;
    int cutoff = a->cutoff, resonance = a->resonance, envMod = a->envMod;
    int decay = a->decay, accent = a->accent;
    int timbre = a->timbre, color = a->color;
    float gain = phraseGain(playback, track, &project->instruments[track->note.instrument]);
    if (track->note.fx[fxACF].isOn) cutoff = instrumentFXCutoff(track->note.fx[fxACF].fxValue);
    if (track->note.fx[fxARS].isOn) resonance = track->note.fx[fxARS].fxValue * 100 / 255;
    if (track->note.fx[fxAEM].isOn) envMod = track->note.fx[fxAEM].fxValue * 100 / 255;
    if (track->note.fx[fxADC].isOn) decay = 200 + track->note.fx[fxADC].fxValue * 1800 / 255;
    if (track->note.fx[fxAAC].isOn) accent = track->note.fx[fxAAC].fxValue * 100 / 255;
    if (a->wave == AChChidWave::braids) {
      timbre = slewEngineFX(track, fxATM, track->note.fx[fxATM].isOn ? track->note.fx[fxATM].fxValue : timbre / 129) * 129;
      color = slewEngineFX(track, fxACL, track->note.fx[fxACL].isOn ? track->note.fx[fxACL].fxValue : color / 129) * 129;
    }
    for (int i = 0; i < 4; ++i) {
      PlaybackModState* mod = &track->note.modulation[i];
      if (!mod->modulation) continue;
      switch (mod->modulation->destination) {
        case 1: { int value = playbackModScaleToRange(mod->outValue, 255); gain = modulationIsAdditive(mod->modulation->type) ? gain + value / 255.0f : value / 255.0f; break; }
        case 3: cutoff = playbackModulateCutoff(cutoff, mod); break;
        case 4: resonance += playbackModScaleToRange(mod->outValue, 100); break;
        case 5: envMod += playbackModScaleToRange(mod->outValue, 100); break;
        case 6: decay += playbackModScaleToRange(mod->outValue, 1800); break;
        case 7: accent += playbackModScaleToRange(mod->outValue, 100); break;
        case 8: if (a->wave == AChChidWave::braids) timbre += playbackModScaleToRange(mod->outValue, 32767); break;
        case 9: if (a->wave == AChChidWave::braids) color += playbackModScaleToRange(mod->outValue, 32767); break;
      }
    }
    for (int slot = 0; slot < track->chordVoiceCount; ++slot)
      voices[slot]->configure((uint8_t)a->wave, a->fineTune, a->model, (uint16_t)clampInt(timbre, 0, 32767), (uint16_t)clampInt(color, 0, 32767), a->saturation,
        (uint16_t)clampInt(cutoff, 200, FILTER_CUTOFF_MAX_HZ), (uint8_t)clampInt(resonance, 0, 100),
        (uint8_t)clampInt(envMod, 0, 100), (uint16_t)clampInt(decay, 200, 2000),
        (uint8_t)clampInt(accent, 0, 100), (gain < 0.0f ? 0.0f : gain) / track->chordVoiceCount);
  }
}

static void updateDrumSynthVoices(ChipNomadState* state) {
  Project* project = &state->audioProject;
  PlaybackState* playback = &state->playbackState;
  for (int trackIdx = 0; trackIdx < project->tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];
    DrumSynthVoice** voices = state->drumSynthVoices[trackIdx];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built
    if (track->note.instrument == EMPTY_VALUE_8 ||
        project->instruments[track->note.instrument].type != InstrumentType::DrumSynth) {
      for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) voices[slot]->kill();
      continue;
    }
    InstrumentDrumSynth* d = &project->instruments[track->note.instrument].chip.drumSynth;
    int engine = (int)d->engine, decay = d->decay, tone = d->tone, sweep = d->sweep;
    int noise = d->noise, fm = d->fm, drive = d->drive;
    int cutoff = d->filterCutoffHz, resonance = d->filterResonance, pitchModulation = 0;
    float gain = phraseGain(playback, track, &project->instruments[track->note.instrument]);
    if (track->note.fx[fxDMD].isOn) engine = track->note.fx[fxDMD].fxValue;
    decay = slewEngineFX(track, fxDDC, track->note.fx[fxDDC].isOn ? track->note.fx[fxDDC].fxValue : decay);
    tone = slewEngineFX(track, fxDTO, track->note.fx[fxDTO].isOn ? track->note.fx[fxDTO].fxValue : tone);
    sweep = slewEngineFX(track, fxDSW, track->note.fx[fxDSW].isOn ? track->note.fx[fxDSW].fxValue : sweep);
    noise = slewEngineFX(track, fxDNO, track->note.fx[fxDNO].isOn ? track->note.fx[fxDNO].fxValue : noise);
    fm = slewEngineFX(track, fxDFM, track->note.fx[fxDFM].isOn ? track->note.fx[fxDFM].fxValue : fm);
    drive = slewEngineFX(track, fxDDR, track->note.fx[fxDDR].isOn ? track->note.fx[fxDDR].fxValue : drive);
    cutoff = instrumentFXCutoff(slewEngineFX(track, fxDCF, track->note.fx[fxDCF].isOn ? track->note.fx[fxDCF].fxValue : filterControlFromCutoff(cutoff)));
    resonance = slewEngineFX(track, fxDRS, track->note.fx[fxDRS].isOn ? track->note.fx[fxDRS].fxValue : resonance);
    for (int i = 0; i < 4; ++i) {
      PlaybackModState* mod = &track->note.modulation[i]; if (!mod->modulation) continue;
      int value = playbackModScaleToRange(mod->outValue, 255);
      switch (mod->modulation->destination) {
        case 1: gain = modulationIsAdditive(mod->modulation->type) ? gain + value / 255.0f : value * track->note.volume / (255.0f * PHRASE_VOLUME_MAX); break;
        case 2: pitchModulation += playbackModScaleToRange(mod->outValue, 1200); break;
        case 3: decay += value; break; case 4: tone += value; break; case 5: sweep += value; break;
        case 6: noise += value; break; case 7: fm += value; break; case 8: drive += value; break;
        case 9: cutoff = playbackModulateCutoff(cutoff, mod); break; case 10: resonance += value; break;
      }
    }
    InstrumentDrumSynth configured = *d;
    configured.engine = (DrumSynthEngine)clampInt(engine, 0, 11);
    configured.decay = (uint8_t)clampInt(decay, 0, 255); configured.tone = (uint8_t)clampInt(tone, 0, 255);
    configured.sweep = (uint8_t)clampInt(sweep, 0, 255); configured.noise = (uint8_t)clampInt(noise, 0, 255);
    configured.fm = (uint8_t)clampInt(fm, 0, 255); configured.drive = (uint8_t)clampInt(drive, 0, 255);
    for (int slot = 0; slot < track->chordVoiceCount; ++slot) {
      uint8_t note = track->chordPitchFinal[slot];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) + track->note.fineOffset + pitchModulation;
      voices[slot]->configure(&configured, (float)cents, (gain < 0.0f ? 0.0f : gain) / track->chordVoiceCount,
        (uint16_t)clampInt(cutoff, FILTER_CUTOFF_MIN_HZ, FILTER_CUTOFF_MAX_HZ), (uint8_t)clampInt(resonance, 0, 255));
    }
  }
}

static void updateMMEVoices(ChipNomadState* state) {
  Project* project = &state->audioProject;
  PlaybackState* playback = &state->playbackState;
  for (int trackIdx = 0; trackIdx < project->tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];
    MMEVoice** voices = state->mmeVoices[trackIdx];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built
    if (track->note.instrument == EMPTY_VALUE_8 ||
        project->instruments[track->note.instrument].type != InstrumentType::MME) { for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) voices[slot]->kill(); continue; }
    InstrumentMME* m = &project->instruments[track->note.instrument].chip.mme;
    int model = (int)m->model, waves = m->waves, interval = m->interval, amount = m->amount;
    int flow = m->flow, feedback = m->feedback, shaper = m->shaper;
    int cutoff = m->filterCutoffHz, resonance = m->filterResonance, pitch = 0;
    float gain = phraseGain(playback, track, &project->instruments[track->note.instrument]);
    if (track->note.fx[fxMMD].isOn) model = track->note.fx[fxMMD].fxValue;
    waves = slewEngineFX(track, fxMWV, track->note.fx[fxMWV].isOn ? track->note.fx[fxMWV].fxValue : waves);
    interval = slewEngineFX(track, fxMIN, track->note.fx[fxMIN].isOn ? track->note.fx[fxMIN].fxValue : interval);
    amount = slewEngineFX(track, fxMAM, track->note.fx[fxMAM].isOn ? track->note.fx[fxMAM].fxValue : amount);
    flow = slewEngineFX(track, fxMFL, track->note.fx[fxMFL].isOn ? track->note.fx[fxMFL].fxValue : flow);
    feedback = slewEngineFX(track, fxMFB, track->note.fx[fxMFB].isOn ? track->note.fx[fxMFB].fxValue : feedback);
    shaper = slewEngineFX(track, fxMSH, track->note.fx[fxMSH].isOn ? track->note.fx[fxMSH].fxValue : shaper);
    cutoff = instrumentFXCutoff(slewEngineFX(track, fxMCF, track->note.fx[fxMCF].isOn ? track->note.fx[fxMCF].fxValue : filterControlFromCutoff(cutoff)));
    resonance = slewEngineFX(track, fxMRS, track->note.fx[fxMRS].isOn ? track->note.fx[fxMRS].fxValue : resonance);
    for (int i = 0; i < 4; ++i) {
      PlaybackModState* mod = &track->note.modulation[i]; if (!mod->modulation) continue;
      int value = playbackModScaleToRange(mod->outValue, 255);
      switch (mod->modulation->destination) {
        case 1: gain = modulationIsAdditive(mod->modulation->type) ? gain + value / 255.0f : value / 255.0f; break;
        case 2: pitch += playbackModScaleToRange(mod->outValue, 1200); break;
        case 3: waves += value; break; case 4: interval += value; break; case 5: amount += value; break;
        case 6: flow += value; break; case 7: feedback += value; break; case 8: shaper += value; break;
        case 9: cutoff = playbackModulateCutoff(cutoff, mod); break; case 10: resonance += value; break;
      }
    }
    InstrumentMME configured = *m;
    configured.model = (MMEModel)clampInt(model, 0, (int)MMEModel::totalCount - 1);
    configured.waves = (uint8_t)clampInt(waves, 0, 255); configured.interval = (uint8_t)clampInt(interval, 0, 255);
    configured.amount = (uint8_t)clampInt(amount, 0, 255); configured.flow = (uint8_t)clampInt(flow, 0, 255);
    configured.feedback = (uint8_t)clampInt(feedback, 0, 255); configured.shaper = (uint8_t)clampInt(shaper, 0, 255);
    int attack = configured.attack, decay = configured.decay, sustain = configured.sustain, release = configured.release;
    int shape = configured.envelopeShape, triggerDecay = 0, triggerColor = 0;
    if (track->note.fx[fxEAT].isOn) attack = track->note.fx[fxEAT].fxValue;
    if (track->note.fx[fxEDC].isOn) decay = track->note.fx[fxEDC].fxValue;
    if (track->note.fx[fxESU].isOn) sustain = track->note.fx[fxESU].fxValue;
    if (track->note.fx[fxERL].isOn) release = track->note.fx[fxERL].fxValue;
    if (track->note.fx[fxESH].isOn) shape = track->note.fx[fxESH].fxValue;
    applyVoicePostModulations(track, InstrumentType::MME, &attack, &decay, &sustain, &release,
                              &shape, &triggerDecay, &triggerColor);
    configured.attack = (uint8_t)attack; configured.decay = (uint8_t)decay; configured.sustain = (uint8_t)sustain;
    configured.release = (uint8_t)release; configured.envelopeShape = (uint8_t)shape;
    for (int slot = 0; slot < track->chordVoiceCount; ++slot) {
      uint8_t note = track->chordPitchFinal[slot];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) + track->note.fineOffset + pitch;
      voices[slot]->configure(&configured, (float)cents, (gain < 0.0f ? 0.0f : gain) / track->chordVoiceCount,
        (uint16_t)clampInt(cutoff, FILTER_CUTOFF_MIN_HZ, FILTER_CUTOFF_MAX_HZ), (uint8_t)clampInt(resonance, 0, 255));
    }
  }
}

static void updateSinteredVoices(ChipNomadState* state) {
  Project* project = &state->audioProject;
  PlaybackState* playback = &state->playbackState;
  for (int trackIdx = 0; trackIdx < project->tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];
    SinteredVoice** voices = state->sinteredVoices[trackIdx];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built
    if (track->note.instrument == EMPTY_VALUE_8 ||
        project->instruments[track->note.instrument].type != InstrumentType::Sintered) { for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) voices[slot]->kill(); continue; }
    InstrumentSintered* s = &project->instruments[track->note.instrument].chip.sintered;
    int model = (int)s->model, decay = s->decay, mod = s->mod, a = s->a, b = s->b, motion = s->motion, c = s->c;
    int cutoff = s->filterCutoffHz, resonance = s->filterResonance, pitch = 0;
    float gain = phraseGain(playback, track, &project->instruments[track->note.instrument]);
    if (track->note.fx[fxSMDL].isOn) model = track->note.fx[fxSMDL].fxValue;
    decay = slewEngineFX(track, fxSDC, track->note.fx[fxSDC].isOn ? track->note.fx[fxSDC].fxValue : decay);
    mod = slewEngineFX(track, fxSMD, track->note.fx[fxSMD].isOn ? track->note.fx[fxSMD].fxValue : mod);
    a = slewEngineFX(track, fxSA, track->note.fx[fxSA].isOn ? track->note.fx[fxSA].fxValue : a);
    b = slewEngineFX(track, fxSB, track->note.fx[fxSB].isOn ? track->note.fx[fxSB].fxValue : b);
    motion = slewEngineFX(track, fxSMO, track->note.fx[fxSMO].isOn ? track->note.fx[fxSMO].fxValue : motion);
    c = slewEngineFX(track, fxSC, track->note.fx[fxSC].isOn ? track->note.fx[fxSC].fxValue : c);
    cutoff = instrumentFXCutoff(slewEngineFX(track, fxSCF3, track->note.fx[fxSCF3].isOn ? track->note.fx[fxSCF3].fxValue : filterControlFromCutoff(cutoff)));
    resonance = slewEngineFX(track, fxSRS3, track->note.fx[fxSRS3].isOn ? track->note.fx[fxSRS3].fxValue : resonance);
    for (int i = 0; i < 4; ++i) {
      PlaybackModState* stateMod = &track->note.modulation[i]; if (!stateMod->modulation) continue;
      int value = playbackModScaleToRange(stateMod->outValue, 255);
      switch (stateMod->modulation->destination) {
        case 1: gain = modulationIsAdditive(stateMod->modulation->type) ? gain + value / 255.0f : value / 255.0f; break;
        case 2: pitch += playbackModScaleToRange(stateMod->outValue, 1200); break;
        case 3: decay += value; break; case 4: mod += value; break; case 5: a += value; break;
        case 6: b += value; break; case 7: motion += value; break; case 8: c += value; break;
        case 9: cutoff = playbackModulateCutoff(cutoff, stateMod); break; case 10: resonance += value; break;
      }
    }
    InstrumentSintered configured = *s;
    configured.model = (SinteredModel)clampInt(model, 0, (int)SinteredModel::totalCount - 1);
    configured.decay = (uint8_t)clampInt(decay, 0, 255); configured.mod = (uint8_t)clampInt(mod, 0, 255);
    configured.a = (uint8_t)clampInt(a, 0, 255); configured.b = (uint8_t)clampInt(b, 0, 255);
    configured.motion = (uint8_t)clampInt(motion, 0, 255); configured.c = (uint8_t)clampInt(c, 0, 255);
    for (int slot = 0; slot < track->chordVoiceCount; ++slot) {
      uint8_t note = track->chordPitchFinal[slot];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) + track->note.fineOffset + pitch;
      voices[slot]->configure(&configured, (float)cents, (gain < 0.0f ? 0.0f : gain) / track->chordVoiceCount,
        (uint16_t)clampInt(cutoff, FILTER_CUTOFF_MIN_HZ, FILTER_CUTOFF_MAX_HZ), (uint8_t)clampInt(resonance, 0, 255));
    }
  }
}

static void updateBraidsVoices(ChipNomadState* state) {
  Project* project = &state->audioProject;
  PlaybackState* playback = &state->playbackState;

  for (int trackIdx = 0; trackIdx < project->tracksCount; trackIdx++) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];
    BraidsVoice** voices = state->braidsVoices[trackIdx];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built

    if (track->note.instrument == EMPTY_VALUE_8 ||
        project->instruments[track->note.instrument].type != InstrumentType::Braids) {
      for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) voices[slot]->kill();
      continue;
    }

    InstrumentBraids* instrument = &project->instruments[track->note.instrument].chip.braids;
    int timbre = instrument->timbre;
    int color = instrument->color;
    int cutoff = instrument->filterCutoffHz;
    int resonance = instrument->filterResonance;
    int model = instrument->model;
    int pitchModulation = 0;
    int attack = instrument->attack, decay = instrument->decay, sustain = instrument->sustain;
    int release = instrument->release, shape = instrument->envelopeShape;
    int triggerDecay = decay, triggerColor = sustain;
    float gain = phraseGain(playback, track, &project->instruments[track->note.instrument]);

    if (track->note.fx[fxBMD].isOn) {
      model = clampInt(track->note.fx[fxBMD].fxValue, 0,
        braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META);
    }
    timbre = slewEngineFX(track, fxBTM, track->note.fx[fxBTM].isOn ? track->note.fx[fxBTM].fxValue : timbre / 129) * 129;
    color = slewEngineFX(track, fxBCL, track->note.fx[fxBCL].isOn ? track->note.fx[fxBCL].fxValue : color / 129) * 129;
    cutoff = instrumentFXCutoff(slewEngineFX(track, fxBCF, track->note.fx[fxBCF].isOn ? track->note.fx[fxBCF].fxValue : filterControlFromCutoff(cutoff)));
    resonance = slewEngineFX(track, fxBRS, track->note.fx[fxBRS].isOn ? track->note.fx[fxBRS].fxValue : resonance);

    for (int i = 0; i < 4; i++) {
      PlaybackModState* mod = &track->note.modulation[i];
      if (!mod->modulation) continue;
      switch (mod->modulation->destination) {
        case 1: {
          int value = playbackModScaleToRange(mod->outValue, 255);
          gain = modulationIsAdditive(mod->modulation->type)
            ? gain + value / 255.0f : value * track->note.volume / (255.0f * PHRASE_VOLUME_MAX);
          break;
        }
        case 2: pitchModulation += playbackModScaleToRange(mod->outValue, 1200); break;
        case 3: timbre += playbackModScaleToRange(mod->outValue, 32767); break;
        case 4: color += playbackModScaleToRange(mod->outValue, 32767); break;
        case 5: cutoff = playbackModulateCutoff(cutoff, mod); break;
        case 6: resonance += playbackModScaleToRange(mod->outValue, 255); break;
      }
    }

    timbre = clampInt(timbre, 0, 32767);
    color = clampInt(color, 0, 32767);
    cutoff = clampInt(cutoff, FILTER_CUTOFF_MIN_HZ, FILTER_CUTOFF_MAX_HZ);
    resonance = clampInt(resonance, 0, 255);
    if (track->note.fx[fxEAT].isOn) attack = track->note.fx[fxEAT].fxValue;
    if (track->note.fx[fxEDC].isOn) decay = track->note.fx[fxEDC].fxValue;
    if (track->note.fx[fxESU].isOn) sustain = track->note.fx[fxESU].fxValue;
    if (track->note.fx[fxERL].isOn) release = track->note.fx[fxERL].fxValue;
    if (track->note.fx[fxESH].isOn) shape = track->note.fx[fxESH].fxValue;
    applyVoicePostModulations(track, InstrumentType::Braids, &attack, &decay, &sustain, &release,
                              &shape, &triggerDecay, &triggerColor);

    for (int slot = 0; slot < track->chordVoiceCount; ++slot) {
      BraidsVoice* voice = voices[slot];
      voice->setModel(model);
      voice->setParameters(timbre, color);
      voice->setGain(gain / track->chordVoiceCount);
      voice->setFilter(
        instrument->filterEnabled != 0,
        instrument->filterCharacter,
        static_cast<BraidsFilterMode>(instrument->filterMode > 2 ? 0 : instrument->filterMode),
        instrument->filterSlope24dB != 0,
        cutoff,
        resonance / 255.0f);
      voice->setEnvelope(true,
        envelopeTime(attack), envelopeTime(decay), sustain / 255.0f, envelopeTime(release), shape);
      uint8_t note = track->chordPitchFinal[slot];
      if (note != EMPTY_VALUE_8) {
        int cents = (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100)
          + track->note.fineOffset + pitchModulation;
        voice->setPitch(static_cast<int16_t>((cents * 128) / 100));
      }
    }

  }
}

static void updatePlaitsVoices(ChipNomadState* state) {
  Project* project = &state->audioProject;
  PlaybackState* playback = &state->playbackState;
  for (int trackIdx = 0; trackIdx < project->tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];
    PlaitsVoice** voices = state->plaitsVoices[trackIdx];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built
    if (track->note.instrument == EMPTY_VALUE_8 ||
        project->instruments[track->note.instrument].type != InstrumentType::Plaits) {
      for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) voices[slot]->kill();
      continue;
    }

    InstrumentPlaits* p = &project->instruments[track->note.instrument].chip.plaits;
    int engine = p->engine;
    int harmonics = p->harmonics;
    int timbre = p->timbre;
    int morph = p->morph;
    int auxMix = p->auxMix;
    int cutoff = p->filterCutoffHz;
    int resonance = p->filterResonance;
    int pitchModulation = 0;
    int attack = p->attack, decay = p->decay, sustain = p->sustain;
    int release = p->release, shape = p->envelopeShape;
    int triggerDecay = decay, triggerColor = sustain;
    float gain = phraseGain(playback, track, &project->instruments[track->note.instrument]);

    if (track->note.fx[fxPMD].isOn) engine = track->note.fx[fxPMD].fxValue;
    harmonics = slewEngineFX(track, fxPHA, track->note.fx[fxPHA].isOn ? track->note.fx[fxPHA].fxValue : harmonics / 129) * 129;
    timbre = slewEngineFX(track, fxPTM, track->note.fx[fxPTM].isOn ? track->note.fx[fxPTM].fxValue : timbre / 129) * 129;
    morph = slewEngineFX(track, fxPMO, track->note.fx[fxPMO].isOn ? track->note.fx[fxPMO].fxValue : morph / 129) * 129;
    auxMix = slewEngineFX(track, fxPAX, track->note.fx[fxPAX].isOn ? track->note.fx[fxPAX].fxValue : auxMix);
    cutoff = instrumentFXCutoff(slewEngineFX(track, fxPCF, track->note.fx[fxPCF].isOn ? track->note.fx[fxPCF].fxValue : filterControlFromCutoff(cutoff)));
    resonance = slewEngineFX(track, fxPRS, track->note.fx[fxPRS].isOn ? track->note.fx[fxPRS].fxValue : resonance);

    for (int i = 0; i < 4; ++i) {
      PlaybackModState* mod = &track->note.modulation[i];
      if (!mod->modulation) continue;
      int value = playbackModScaleToRange(mod->outValue, 255);
      switch (mod->modulation->destination) {
        case 1: gain = modulationIsAdditive(mod->modulation->type) ? gain + value / 255.0f : value * track->note.volume / (255.0f * PHRASE_VOLUME_MAX); break;
        case 2: pitchModulation += playbackModScaleToRange(mod->outValue, 1200); break;
        case 3: harmonics += playbackModScaleToRange(mod->outValue, 32767); break;
        case 4: timbre += playbackModScaleToRange(mod->outValue, 32767); break;
        case 5: morph += playbackModScaleToRange(mod->outValue, 32767); break;
        case 6: auxMix += value; break;
        case 7: cutoff = playbackModulateCutoff(cutoff, mod); break;
        case 8: resonance += value; break;
      }
    }

    engine = clampInt(engine, 0, 23);
    harmonics = clampInt(harmonics, 0, 32767);
    timbre = clampInt(timbre, 0, 32767);
    morph = clampInt(morph, 0, 32767);
    auxMix = clampInt(auxMix, 0, 255);
    cutoff = clampInt(cutoff, FILTER_CUTOFF_MIN_HZ, FILTER_CUTOFF_MAX_HZ);
    resonance = clampInt(resonance, 0, 255);
    if (track->note.fx[fxEAT].isOn) attack = track->note.fx[fxEAT].fxValue;
    if (track->note.fx[fxEDC].isOn) decay = track->note.fx[fxEDC].fxValue;
    if (track->note.fx[fxESU].isOn) sustain = track->note.fx[fxESU].fxValue;
    if (track->note.fx[fxERL].isOn) release = track->note.fx[fxERL].fxValue;
    if (track->note.fx[fxESH].isOn) shape = track->note.fx[fxESH].fxValue;
    if (p->envelopeMode == 0) {
      if (track->note.fx[fxTDC].isOn) triggerDecay = track->note.fx[fxTDC].fxValue;
      if (track->note.fx[fxTCL].isOn) triggerColor = track->note.fx[fxTCL].fxValue;
    }
    applyVoicePostModulations(track, InstrumentType::Plaits, &attack, &decay, &sustain, &release,
                              &shape, &triggerDecay, &triggerColor);
    for (int slot = 0; slot < track->chordVoiceCount; ++slot) {
      uint8_t note = track->chordPitchFinal[slot];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) +
        track->note.fineOffset + pitchModulation;
      PlaitsVoice* voice = voices[slot];
      voice->configure((uint8_t)engine, (uint16_t)harmonics, (uint16_t)timbre,
                       (uint16_t)morph, (uint8_t)auxMix, p->envelopeMode,
                       triggerDecay, triggerColor, cents / 100.0f, gain / track->chordVoiceCount);
      voice->setFilter(p->filterEnabled != 0, p->filterCharacter, p->filterMode, p->filterSlope24dB != 0,
                       cutoff, resonance / 255.0f);
      voice->setEnvelope(envelopeTime(attack), envelopeTime(decay),
                         sustain / 255.0f, envelopeTime(release), shape);
    }
  }
}

static void updatePlaitsAltVoices(ChipNomadState* state) {
  Project* project = &state->audioProject;
  PlaybackState* playback = &state->playbackState;
  for (int trackIdx = 0; trackIdx < project->tracksCount; ++trackIdx) {
    PlaybackTrackState* track = &playback->tracks[trackIdx];
    PlaitsAltVoice** voices = state->plaitsAltVoices[trackIdx];
    if (!gateLoad(&voices[0])) continue;  // lazy: set not built
    if (track->note.instrument == EMPTY_VALUE_8 ||
        project->instruments[track->note.instrument].type != InstrumentType::PlaitsAlt) {
      for (int slot = 0; slot < CHORD_MAX_VOICES; ++slot) voices[slot]->kill();
      continue;
    }

    InstrumentPlaits* p = &project->instruments[track->note.instrument].chip.plaits;
    int engine = p->engine, harmonics = p->harmonics, timbre = p->timbre;
    int morph = p->morph, auxMix = p->auxMix, cutoff = p->filterCutoffHz;
    int resonance = p->filterResonance, pitchModulation = 0;
    int attack = p->attack, decay = p->decay, sustain = p->sustain;
    int release = p->release, shape = p->envelopeShape;
    int triggerDecay = decay, triggerColor = sustain;
    float gain = phraseGain(playback, track, &project->instruments[track->note.instrument]);
    if (track->note.fx[fxPMD].isOn) engine = track->note.fx[fxPMD].fxValue;
    harmonics = slewEngineFX(track, fxPHA, track->note.fx[fxPHA].isOn ? track->note.fx[fxPHA].fxValue : harmonics / 129) * 129;
    timbre = slewEngineFX(track, fxPTM, track->note.fx[fxPTM].isOn ? track->note.fx[fxPTM].fxValue : timbre / 129) * 129;
    morph = slewEngineFX(track, fxPMO, track->note.fx[fxPMO].isOn ? track->note.fx[fxPMO].fxValue : morph / 129) * 129;
    auxMix = slewEngineFX(track, fxPAX, track->note.fx[fxPAX].isOn ? track->note.fx[fxPAX].fxValue : auxMix);
    cutoff = instrumentFXCutoff(slewEngineFX(track, fxPCF, track->note.fx[fxPCF].isOn ? track->note.fx[fxPCF].fxValue : filterControlFromCutoff(cutoff)));
    resonance = slewEngineFX(track, fxPRS, track->note.fx[fxPRS].isOn ? track->note.fx[fxPRS].fxValue : resonance);
    for (int i = 0; i < 4; ++i) {
      PlaybackModState* mod = &track->note.modulation[i];
      if (!mod->modulation) continue;
      int value = playbackModScaleToRange(mod->outValue, 255);
      switch (mod->modulation->destination) {
        case 1: gain = modulationIsAdditive(mod->modulation->type) ? gain + value / 255.0f : value * track->note.volume / (255.0f * PHRASE_VOLUME_MAX); break;
        case 2: pitchModulation += playbackModScaleToRange(mod->outValue, 1200); break;
        case 3: harmonics += playbackModScaleToRange(mod->outValue, 32767); break;
        case 4: timbre += playbackModScaleToRange(mod->outValue, 32767); break;
        case 5: morph += playbackModScaleToRange(mod->outValue, 32767); break;
        case 6: auxMix += value; break;
        case 7: cutoff = playbackModulateCutoff(cutoff, mod); break;
        case 8: resonance += value; break;
      }
    }
    engine = clampInt(engine, 0, 23); harmonics = clampInt(harmonics, 0, 32767);
    timbre = clampInt(timbre, 0, 32767); morph = clampInt(morph, 0, 32767);
    auxMix = clampInt(auxMix, 0, 255); cutoff = clampInt(cutoff, FILTER_CUTOFF_MIN_HZ, FILTER_CUTOFF_MAX_HZ);
    resonance = clampInt(resonance, 0, 255);
    if (track->note.fx[fxEAT].isOn) attack = track->note.fx[fxEAT].fxValue;
    if (track->note.fx[fxEDC].isOn) decay = track->note.fx[fxEDC].fxValue;
    if (track->note.fx[fxESU].isOn) sustain = track->note.fx[fxESU].fxValue;
    if (track->note.fx[fxERL].isOn) release = track->note.fx[fxERL].fxValue;
    if (track->note.fx[fxESH].isOn) shape = track->note.fx[fxESH].fxValue;
    if (p->envelopeMode == 0) {
      if (track->note.fx[fxTDC].isOn) triggerDecay = track->note.fx[fxTDC].fxValue;
      if (track->note.fx[fxTCL].isOn) triggerColor = track->note.fx[fxTCL].fxValue;
    }
    applyVoicePostModulations(track, InstrumentType::PlaitsAlt, &attack, &decay, &sustain, &release,
                              &shape, &triggerDecay, &triggerColor);
    for (int slot = 0; slot < track->chordVoiceCount; ++slot) {
      uint8_t note = track->chordPitchFinal[slot];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) +
        track->note.fineOffset + pitchModulation;
      PlaitsAltVoice* voice = voices[slot];
      voice->configure((uint8_t)engine, (uint16_t)harmonics, (uint16_t)timbre,
        (uint16_t)morph, (uint8_t)auxMix, p->envelopeMode, triggerDecay, triggerColor,
        cents / 100.0f, gain / track->chordVoiceCount);
      voice->setFilter(p->filterEnabled != 0, p->filterCharacter, p->filterMode, p->filterSlope24dB != 0,
        cutoff, resonance / 255.0f);
      voice->setEnvelope(envelopeTime(attack), envelopeTime(decay),
        sustain / 255.0f, envelopeTime(release), shape);
    }
  }
}

static void detectAYPitchConflicts(ChipNomadState* state) {
  // Independent AY instances cannot fight over a shared tone generator.
  for (int i = 0; i < PROJECT_MAX_TRACKS; i++) state->trackWarnings[i] = 0;
}

void chipnomadSetQuality(ChipNomadState* state, ChipNomadQuality quality) {
  for (int i = 0; i < PROJECT_MAX_CHIPS; i++) {
    if (state->chips[i]) {
      state->chips[i]->setQuality(quality);
    }
  }
}

void chipnomadSetBraidsSettings(ChipNomadState* state, uint8_t bits,
                               uint8_t drift, uint8_t signature,
                               uint32_t signatureSeed) {
  if (!state) return;
  // Remembered so braids sets built later (lazy modes) get the same settings.
  state->braidsSettingsValid = 1;
  state->braidsBits = bits; state->braidsDrift = drift;
  state->braidsSignature = signature; state->braidsSeed = signatureSeed;
  for (int i = 0; i < PROJECT_MAX_TRACKS; ++i) {
    if (!state->braidsVoices[i][0]) continue;
    for (int voice = 0; voice < CHORD_MAX_VOICES; ++voice)
      state->braidsVoices[i][voice]->setGlobalSettings(bits, drift, signature,
        signatureSeed);
  }
}


static bool nativeControlActive(const PlaybackTrackState* track,const Instrument* instrument,int generic) {
  const auto* d=instrumentNativeModDestination(instrument->type,generic);
  if(!d)return false;
  if(track->note.fx[d->fx].isOn)return true;
  for(const auto& mod:track->note.modulation)if(mod.modulation&&instrumentGenericModDestination(instrument->type,mod.modulation->destination)==generic)return true;
  return false;
}

static int nativeControlValue(PlaybackTrackState* track,const Instrument* instrument,int generic) {
  const auto* d=instrumentNativeModDestination(instrument->type,generic);
  if(!d)return 0;
  int value=track->note.fx[d->fx].isOn?track->note.fx[d->fx].fxValue:instrumentNativeControlValue(instrument,generic);
  if(generic==genericModFMBrightness||(generic>=genericModFMOperator1&&generic<=genericModFMOperator6)||(generic>=genericModFMTime&&generic<=genericModFMLFODepth)||generic==genericModSIDPulse||generic==genericModSIDCutoff)value=slewEngineFX(track,FX(d->fx),value);
  for(const auto& mod:track->note.modulation) {
    if(!mod.modulation||instrumentGenericModDestination(instrument->type,mod.modulation->destination)!=generic)continue;
    int amount=playbackModScaleToRange(mod.outValue,d->range);
    value=modulationIsAdditive(mod.modulation->type)?value+amount:amount;
  }
  return clampInt(value,0,d->range);
}

static void configureOperatorLevels(InstrumentFMTone& tone,PlaybackTrackState* track,const Instrument* instrument) {
  auto advance=[&](uint16_t target,uint16_t& current,uint16_t& remaining) {
    if(!target)return;
    if(!track->slewTicks||!remaining)current=target;
    else { int distance=int(target)-current;int step=distance/int(remaining);
      if(!step&&distance)step=distance<0?-1:1;
      current+=step;--remaining;
    }
  };
  for(int op=0;op<6;++op)for(int p=0;p<12;++p)
    advance(track->note.nativeFM.operators[op][p],track->note.nativeFMCurrent.operators[op][p],track->note.nativeFMRemaining.operators[op][p]);
  for(int p=0;p<6;++p)advance(track->note.nativeFM.global[p],track->note.nativeFMCurrent.global[p],track->note.nativeFMRemaining.global[p]);
  tone.direct = track->note.nativeFMCurrent;
  for(int op=0;op<6;++op) {
    tone.operatorLevel[op]=0;
    auto fx=FX(fxOL1+op);NativeFXInfo info{};
    if(track->note.fx[fx].isOn&&instrumentNativeFXInfo(instrument,fx,&info))
      tone.operatorLevel[op]=clampInt(slewEngineFX(track,fx,track->note.fx[fx].fxValue),0,info.maximum)+1;
  }
  for(const auto& mod:track->note.modulation) {
    if(!mod.modulation)continue;
    int g=instrumentGenericModDestination(instrument->type,mod.modulation->destination),fx,op;
    if(!nativeFMModTarget(g,&fx,&op))continue;
    NativeFXInfo info{};if(!instrumentNativeFXInfo(instrument,fx,&info,op))continue;
    uint16_t* target=fx>=fxOL1&&fx<=fxOL6?nullptr:fx>=fxLFR?&tone.direct.global[fx-fxLFR]:&tone.direct.operators[op][fx-fxOAR];
    int base=target?(*target?*target-1:info.preset):(tone.operatorLevel[op]?tone.operatorLevel[op]-1:info.preset);
    int amount=playbackModScaleToRange(mod.outValue,info.maximum);
    int value=clampInt(modulationIsAdditive(mod.modulation->type)?base+amount:amount,info.minimum,info.maximum);
    if(target)*target=value+1;else tone.operatorLevel[op]=value+1;
  }
}

static void configureFMAmp(InstrumentFMAmp& amp, const PlaybackTrackState* track, InstrumentType type) {
  int attack=amp.attack,decay=amp.decay,sustain=amp.sustain,release=amp.release;
  int shape=amp.envelopeShape,triggerDecay=0,triggerColor=0;
  if(track->note.fx[fxEAT].isOn)attack=track->note.fx[fxEAT].fxValue;
  if(track->note.fx[fxEDC].isOn)decay=track->note.fx[fxEDC].fxValue;
  if(track->note.fx[fxESU].isOn)sustain=track->note.fx[fxESU].fxValue;
  if(track->note.fx[fxERL].isOn)release=track->note.fx[fxERL].fxValue;
  if(track->note.fx[fxESH].isOn)shape=track->note.fx[fxESH].fxValue;
  applyVoicePostModulations(track,type,&attack,&decay,&sustain,&release,&shape,&triggerDecay,&triggerColor);
  amp.attack=attack;amp.decay=decay;amp.sustain=sustain;amp.release=release;amp.envelopeShape=shape;
}

static void updateOPLLVoices(ChipNomadState* state) {
  auto* project = &state->audioProject;
  auto* playback = &state->playbackState;
  for (int t = 0; t < project->tracksCount; ++t) {
    auto* track = &playback->tracks[t]; auto* voices = state->opllVoices[t]; if (!gateLoad(&voices[0])) continue;
    if (track->note.instrument == EMPTY_VALUE_8 || !isOPLL(project->instruments[track->note.instrument].type)) {
      for (int v = 0; v < CHORD_MAX_VOICES; ++v) voices[v]->kill();
      continue;
    }
    auto* instrument = &project->instruments[track->note.instrument];
    float gain = phraseGain(playback, track, instrument); int pitch = 0;
    for (auto& mod : track->note.modulation) {
      if (!mod.modulation) continue;
      if (mod.modulation->destination == 1) {
        float value = playbackModScaleToRange(mod.outValue, 255) / 255.0f;
        gain = modulationIsAdditive(mod.modulation->type) ? gain + value : value;
      } else if (mod.modulation->destination == 2) pitch += playbackModScaleToRange(mod.outValue, 1200);
    }
    auto configured=instrument->chip.opll;
    configureFMAmp(configured.amp,track,instrument->type);
    configureOperatorLevels(configured.tone,track,instrument);
    if(nativeControlActive(track,instrument,genericModFMFeedback))configured.tone.feedback=nativeControlValue(track,instrument,genericModFMFeedback)+1;
    for (int v = 0; v < track->chordVoiceCount; ++v) {
      uint8_t note = track->chordPitchFinal[v];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) + track->note.fineOffset + pitch;
      voices[v]->configure(&configured, cents, gain / track->chordVoiceCount);
    }
  }
}

int chipnomadQueueOPLLPreview(ChipNomadState* state, int track, const InstrumentOPLL* patch) {
  if (!state || !state->audioCommands || track < 0 || track >= PROJECT_MAX_TRACKS) return 0;
  if (patch) previewEnsure(state, state->opllPreview);
  return state->audioCommands->pushCommand(11, track, patch ? 1 : 0, 0, 0, nullptr, patch);
}
static void updateOPLVoices(ChipNomadState* state) {
  auto* project = &state->audioProject;
  auto* playback = &state->playbackState;
  for (int t = 0; t < project->tracksCount; ++t) {
    auto* track = &playback->tracks[t]; auto* voices = state->oplVoices[t]; if (!gateLoad(&voices[0])) continue;
    if (track->note.instrument == EMPTY_VALUE_8 || !isOPL(project->instruments[track->note.instrument].type)) {
      for (int v = 0; v < CHORD_MAX_VOICES; ++v) voices[v]->kill();
      continue;
    }
    auto* instrument = &project->instruments[track->note.instrument];
    float gain = phraseGain(playback, track, instrument); int pitch = 0;
    for (auto& mod : track->note.modulation) {
      if (!mod.modulation) continue;
      if (mod.modulation->destination == 1) {
        float value = playbackModScaleToRange(mod.outValue, 255) / 255.0f;
        gain = modulationIsAdditive(mod.modulation->type) ? gain + value : value;
      } else if (mod.modulation->destination == 2) pitch += playbackModScaleToRange(mod.outValue, 1200);
    }
    auto configured=instrument->chip.opl;
    configureFMAmp(configured.amp,track,instrument->type);
    configureOperatorLevels(configured.tone,track,instrument);
    if(nativeControlActive(track,instrument,genericModFMFeedback))configured.tone.feedback=nativeControlValue(track,instrument,genericModFMFeedback)+1;
    for (int v = 0; v < track->chordVoiceCount; ++v) {
      uint8_t note = track->chordPitchFinal[v];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) + track->note.fineOffset + pitch;
      voices[v]->configure(instrument->type, &configured, cents, gain / track->chordVoiceCount);
    }
  }
}

int chipnomadQueueOPLPreview(ChipNomadState* state,int track,InstrumentType type,const InstrumentOPL* patch){
  if(!state||!state->audioCommands||track<0||track>=PROJECT_MAX_TRACKS||!isOPL(type))return 0;
  if (patch) previewEnsure(state, state->oplPreview);
  return state->audioCommands->pushCommand(12,track,patch?1:0,int(type),0,nullptr,nullptr,patch);
}
static void updateFourOpVoices(ChipNomadState* state) {
  auto* project = &state->audioProject;
  auto* playback = &state->playbackState;
  for (int t = 0; t < project->tracksCount; ++t) {
    auto* track = &playback->tracks[t]; auto* voices = state->fourOpVoices[t]; if (!gateLoad(&voices[0])) continue;
    if (track->note.instrument == EMPTY_VALUE_8 || !isFourOp(project->instruments[track->note.instrument].type)) {
      for (int v = 0; v < CHORD_MAX_VOICES; ++v) voices[v]->kill();
      continue;
    }
    auto* instrument = &project->instruments[track->note.instrument];
    float gain = phraseGain(playback, track, instrument); int pitch = 0;
    for (auto& mod : track->note.modulation) {
      if (!mod.modulation) continue;
      if (mod.modulation->destination == 1) {
        float value = playbackModScaleToRange(mod.outValue, 255) / 255.0f;
        gain = modulationIsAdditive(mod.modulation->type) ? gain + value : value;
      } else if (mod.modulation->destination == 2) pitch += playbackModScaleToRange(mod.outValue, 1200);
    }
    auto configured=instrument->chip.fourOp;
    configureFMAmp(configured.amp,track,instrument->type);
    configureOperatorLevels(configured.tone,track,instrument);
    if(nativeControlActive(track,instrument,genericModFMFeedback))configured.tone.feedback=nativeControlValue(track,instrument,genericModFMFeedback)+1;
    for (int v = 0; v < track->chordVoiceCount; ++v) {
      uint8_t note = track->chordPitchFinal[v];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) + track->note.fineOffset + pitch;
      voices[v]->configure(instrument->type, &configured, cents, gain / track->chordVoiceCount);
    }
  }
}

int chipnomadQueueFourOpPreview(ChipNomadState* state,int track,InstrumentType type,const InstrumentFourOp* patch){
  if(!state||!state->audioCommands||track<0||track>=PROJECT_MAX_TRACKS||!isFourOp(type))return 0;
  if (patch) previewEnsure(state, state->fourOpPreview);
  return state->audioCommands->pushCommand(15,track,patch?1:0,int(type),0,nullptr,nullptr,nullptr,nullptr,nullptr,patch);
}
static void updateSimpleChipVoices(ChipNomadState* state) {
  auto* project = &state->audioProject;
  auto* playback = &state->playbackState;
  for (int t = 0; t < project->tracksCount; ++t) {
    auto* track = &playback->tracks[t]; auto* voices = state->simpleChipVoices[t]; if (!gateLoad(&voices[0])) continue;
    if (track->note.instrument == EMPTY_VALUE_8 || !isSimpleChip(project->instruments[track->note.instrument].type)) {
      for (int v = 0; v < CHORD_MAX_VOICES; ++v) voices[v]->kill();
      continue;
    }
    auto* instrument = &project->instruments[track->note.instrument];
    float gain = phraseGain(playback, track, instrument); int pitch = 0;
    for (auto& mod : track->note.modulation) {
      if (!mod.modulation) continue;
      if (mod.modulation->destination == 1) {
        float value = playbackModScaleToRange(mod.outValue, 255) / 255.0f;
        gain = modulationIsAdditive(mod.modulation->type) ? gain + value : value;
      } else if (mod.modulation->destination == 2) pitch += playbackModScaleToRange(mod.outValue, 1200);
    }
    InstrumentSimpleChip configured=instrument->chip.simpleChip;
    configured.mode=nativeControlValue(track,instrument,genericModChipMode);
    if(instrument->type==InstrumentType::SegaPSG)configured.noiseRate=nativeControlValue(track,instrument,genericModChipNoiseRate);
    if(instrument->type==InstrumentType::GBNoise){configured.noiseDivisor=nativeControlValue(track,instrument,genericModChipNoiseDivisor);configured.noiseShift=nativeControlValue(track,instrument,genericModChipNoiseShift);}
    if(instrument->type==InstrumentType::GBPulse){configured.sweepPeriod=nativeControlValue(track,instrument,genericModChipSweepPeriod);configured.sweepShift=nativeControlValue(track,instrument,genericModChipSweepShift);configured.sweepNegate=nativeControlValue(track,instrument,genericModChipSweepDirection);}
    if(instrument->type!=InstrumentType::SegaPSG){configured.envelopeInitial=nativeControlValue(track,instrument,genericModChipEnvelopeInitial);configured.envelopePeriod=nativeControlValue(track,instrument,genericModChipEnvelopePeriod);configured.envelopeIncrease=nativeControlValue(track,instrument,genericModChipEnvelopeDirection);}

    int attack=configured.attack,decay=configured.decay,sustain=configured.sustain,release=configured.release,shape=configured.envelopeShape,triggerDecay=0,triggerColor=0;
    if(track->note.fx[fxEAT].isOn)attack=track->note.fx[fxEAT].fxValue;
    if(track->note.fx[fxEDC].isOn)decay=track->note.fx[fxEDC].fxValue;
    if(track->note.fx[fxESU].isOn)sustain=track->note.fx[fxESU].fxValue;
    if(track->note.fx[fxERL].isOn)release=track->note.fx[fxERL].fxValue;
    if(track->note.fx[fxESH].isOn)shape=track->note.fx[fxESH].fxValue;
    applyVoicePostModulations(track,instrument->type,&attack,&decay,&sustain,&release,&shape,&triggerDecay,&triggerColor);
    configured.attack=attack;configured.decay=decay;configured.sustain=sustain;configured.release=release;configured.envelopeShape=shape;
    for (int v = 0; v < track->chordVoiceCount; ++v) {
      uint8_t note = track->chordPitchFinal[v];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) + track->note.fineOffset + pitch;
      voices[v]->configure(instrument->type, &configured, cents, gain / track->chordVoiceCount);
    }
  }
}

int chipnomadQueueSimpleChipPreview(ChipNomadState* state,int track,InstrumentType type,const InstrumentSimpleChip* patch){
  if(!state||!state->audioCommands||track<0||track>=PROJECT_MAX_TRACKS||!isSimpleChip(type))return 0;
  if (patch) previewEnsure(state, state->simpleChipPreview);
  return state->audioCommands->pushCommand(13,track,patch?1:0,int(type),0,nullptr,nullptr,nullptr,patch);
}

static void updateDX7Voices(ChipNomadState* state) {
  auto* project = &state->audioProject;
  auto* playback = &state->playbackState;
  for (int t = 0; t < project->tracksCount; ++t) {
    auto* track = &playback->tracks[t]; auto* voices = state->dx7Voices[t]; if (!gateLoad(&state->dx7Parts[t])) continue;
    if (track->note.instrument == EMPTY_VALUE_8 || project->instruments[track->note.instrument].type!=InstrumentType::DX7) {
      for (int v = 0; v < CHORD_MAX_VOICES; ++v) voices[v]->kill();
      continue;
    }
    auto* instrument = &project->instruments[track->note.instrument];
    float gain = phraseGain(playback, track, instrument); int pitch = 0;
    for (auto& mod : track->note.modulation) {
      if (!mod.modulation) continue;
      if (mod.modulation->destination == 1) {
        float value = playbackModScaleToRange(mod.outValue, 255) / 255.0f;
        gain = modulationIsAdditive(mod.modulation->type) ? gain + value : value;
      } else if (mod.modulation->destination == 2) pitch += playbackModScaleToRange(mod.outValue, 1200);
    }
    auto configured=instrument->chip.dx7;
    configureFMAmp(configured.amp,track,instrument->type);
    configureOperatorLevels(configured.tone,track,instrument);
    if(nativeControlActive(track,instrument,genericModFMFeedback))configured.tone.feedback=nativeControlValue(track,instrument,genericModFMFeedback)+1;
    for (int v = 0; v < track->chordVoiceCount; ++v) {
      uint8_t note = track->chordPitchFinal[v];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) + track->note.fineOffset + pitch;
      voices[v]->configure(&configured, cents, gain / track->chordVoiceCount);
    }
  }
}

int chipnomadQueueDX7Preview(ChipNomadState* state,int track,const InstrumentDX7* patch) {
  if(!state||!state->audioCommands||track<0||track>=PROJECT_MAX_TRACKS||(patch&&!validDX7(*patch)))return 0;
  if (patch) previewEnsure(state, state->dx7Preview);
  return state->audioCommands->pushCommand(14,track,patch?1:0,0,0,nullptr,nullptr,nullptr,nullptr,patch);
}

static void updateSIDVoices(ChipNomadState* state) {
  auto* project = &state->audioProject;
  auto* playback = &state->playbackState;
  for (int t = 0; t < project->tracksCount; ++t) {
    auto* track = &playback->tracks[t]; auto* voices = state->sidVoices[t]; if (!gateLoad(&voices[0])) continue;
    if (track->note.instrument == EMPTY_VALUE_8 || project->instruments[track->note.instrument].type!=InstrumentType::SID) {
      for (int v = 0; v < CHORD_MAX_VOICES; ++v) voices[v]->kill();
      continue;
    }
    auto* instrument = &project->instruments[track->note.instrument];
    float gain = phraseGain(playback, track, instrument); int pitch = 0;
    for (auto& mod : track->note.modulation) {
      if (!mod.modulation) continue;
      if (mod.modulation->destination == 1) {
        float value = playbackModScaleToRange(mod.outValue, 255) / 255.0f;
        gain = modulationIsAdditive(mod.modulation->type) ? gain + value : value;
      } else if (mod.modulation->destination == 2) pitch += playbackModScaleToRange(mod.outValue, 1200);
    }
    auto configured=instrument->chip.sid;
    // Keep full native precision unless this control actually has an FX/mod override.
    auto changed=[&](int g){const auto* d=instrumentNativeModDestination(instrument->type,g);
      if(track->note.fx[d->fx].isOn)return true;
      for(const auto& m:track->note.modulation)if(m.modulation&&instrumentGenericModDestination(instrument->type,m.modulation->destination)==g)return true;
      return false;
    };
    auto* value=configured.value;
    if(changed(genericModSIDPulse))value[sidPulse]=(nativeControlValue(track,instrument,genericModSIDPulse)*4095+127)/255;
    if(changed(genericModSIDCutoff))value[sidCutoff]=(nativeControlValue(track,instrument,genericModSIDCutoff)*2047+127)/255;
    if(changed(genericModSIDMacroRate))value[sidMacroRate]=std::max(1,nativeControlValue(track,instrument,genericModSIDMacroRate));
    value[sidResonance]=nativeControlValue(track,instrument,genericModSIDResonance);
    value[sidWave]=std::max(1,nativeControlValue(track,instrument,genericModSIDWave));
    value[sidFilterMode]=nativeControlValue(track,instrument,genericModSIDFilterMode);
    value[sidRing]=nativeControlValue(track,instrument,genericModSIDRing);
    value[sidSync]=nativeControlValue(track,instrument,genericModSIDSync);
    value[sidAttack]=nativeControlValue(track,instrument,genericModSIDAttack);
    value[sidDecay]=nativeControlValue(track,instrument,genericModSIDDecay);
    value[sidSustain]=nativeControlValue(track,instrument,genericModSIDSustain);
    value[sidRelease]=nativeControlValue(track,instrument,genericModSIDRelease);
    value[sidPartnerRatio]=std::max(1,nativeControlValue(track,instrument,genericModSIDPartner));
    for (int v = 0; v < track->chordVoiceCount; ++v) {
      uint8_t note = track->chordPitchFinal[v];
      int cents = note == EMPTY_VALUE_8 ? 6000 :
        (project->linearPitch ? project->pitchTable.values[note] : (note + 12) * 100) + track->note.fineOffset + pitch;
      voices[v]->configure(&configured, cents, gain / track->chordVoiceCount);
    }
  }
}

int chipnomadQueueSIDPreview(ChipNomadState* state,int track,const InstrumentSID* patch){
 if(!state||!state->audioCommands||track<0||track>=PROJECT_MAX_TRACKS||(patch&&!validSID(*patch)))return 0;
 if(patch)previewEnsure(state,state->sidPreview);
 return state->audioCommands->pushCommand(16,track,patch?1:0,0,0,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,patch);
}
