#include "master_effects.h"

#include <algorithm>
#include <math.h>

#include "project.h"
#include "fx_profile.h"

static constexpr float tailThreshold = 0.000001f;

namespace {
// Accumulates wall time of a master-effect section into the FX profile.
// Per-sample rendering (fast-LFO projects) times one call in 64 and scales it.
struct ScopedCost {
  int which;
  unsigned weight;
  uint64_t started;
  ScopedCost(int w, size_t frames) : which(w), weight(frames >= 32 ? 1u : ((++tick & 63u) == 0 ? 64u : 0u)),
      started(weight ? fxProfileNowNs() : 0) {}
  ~ScopedCost() { fxProfileMaster(which, weight ? (fxProfileNowNs() - started) * weight : 0); }
  static unsigned tick;
};
unsigned ScopedCost::tick = 0;
}  // namespace

static float mixerGain(uint8_t value) {
  if (value == 0) return 0.0f;
  return powf(10.0f, -60.0f * (100 - value) / 2000.0f);
}

void MasterEffects::init(float sampleRate) {
  sampleRate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
  reverbFrames_.clear();
#ifdef RGNANO_BUILD
  // RG Nano: 2 s of stereo delay line (0.5 MB at 32 kHz) instead of 8 s. The
  // read offset is already clamped to the capacity, so longer delay settings
  // simply top out at 2 s.
  static const float kDelayMaxSeconds = 2.0f;
#else
  static const float kDelayMaxSeconds = 8.0f;
#endif
  const size_t delayFrames = (size_t)(sampleRate_ * kDelayMaxSeconds);
  if (delayLine_.size() != delayFrames * 2) {
    // assign() never gives capacity back, so a rate change (or the old 96 kHz
    // placeholder init) would otherwise keep the larger block alive.
    std::vector<float>().swap(delayLine_);
    delayLine_.assign(delayFrames * 2, 0.0f);
  } else {
    std::fill(delayLine_.begin(), delayLine_.end(), 0.0f);
  }
  springLine_.assign((size_t)(sampleRate_ * 0.12f) * 2, 0.0f);
  reset();
}

void MasterEffects::reserve(size_t frames) {
  reverbFrames_.reserve(frames);
  beadsReverbFrames_.reserve(frames);
}

void MasterEffects::resetDelay() {
  std::fill(delayLine_.begin(), delayLine_.end(), 0.0f);
  std::fill(springLine_.begin(), springLine_.end(), 0.0f);
  delayWrite_ = springWrite_ = 0;
  delayFilterState_[0] = delayFilterState_[1] = 0.0f;
  tapeFilterState_[0] = tapeFilterState_[1] = 0.0f;
  springFilterState_[0] = springFilterState_[1] = 0.0f;
  flutterPhase_ = 0.0f;
  delayTailFrames_ = 0;
}

void MasterEffects::reset() {
  reverb_.Init(reverbMemory_, sampleRate_);
  beadsReverb_.Init(beadsReverbMemory_, sampleRate_);
  resetDelay();
  reverbFilterState_[0] = reverbFilterState_[1] = 0.0f;
  reverbHighpassInputState_[0] = reverbHighpassInputState_[1] = 0.0f;
  reverbHighpassOutputState_[0] = reverbHighpassOutputState_[1] = 0.0f;
  beadsReverbHighpassInputState_[0] = beadsReverbHighpassInputState_[1] = 0.0f;
  beadsReverbHighpassOutputState_[0] = beadsReverbHighpassOutputState_[1] = 0.0f;
  reverbTailFrames_ = 0;
  activeReverbAlgorithm_ = 0xff;
  activeDelayAlgorithm_ = 0xff;
}

bool MasterEffects::tailActive() const {
  return reverbTailFrames_ || delayTailFrames_;
}

float MasterEffects::lowpassCoefficient(float cutoff) const {
  if (cutoff < 20.0f) cutoff = 20.0f;
  if (cutoff > sampleRate_ * 0.45f) cutoff = sampleRate_ * 0.45f;
  return 1.0f - expf(-6.283185307179586f * cutoff / sampleRate_);
}

float MasterEffects::lowpass(float input, float coefficient, float& state) {
  state += coefficient * (input - state);
  return state;
}

float MasterEffects::highpassCoefficient(float cutoff) const {
  if (cutoff < 20.0f) cutoff = 20.0f;
  if (cutoff > sampleRate_ * 0.45f) cutoff = sampleRate_ * 0.45f;
  return expf(-6.283185307179586f * cutoff / sampleRate_);
}

float MasterEffects::highpass(float input, float coefficient, float& inputState, float& outputState) {
  float output = coefficient * (outputState + input - inputState);
  inputState = input;
  return outputState = output;
}

float MasterEffects::delayRead(float delaySamples, int channel) const {
  const size_t capacity = delayLine_.size() / 2;
  if (!capacity) return 0.0f;
  delaySamples = std::max(1.0f, std::min(delaySamples, (float)capacity - 2.0f));
  float read = (float)delayWrite_ - delaySamples;
  while (read < 0.0f) read += capacity;
  while (read >= capacity) read -= capacity;
  size_t first = (size_t)read;
  size_t next = first + 1 == capacity ? 0 : first + 1;
  float fraction = read - (float)first;
  return delayLine_[first * 2 + channel] + fraction *
    (delayLine_[next * 2 + channel] - delayLine_[first * 2 + channel]);
}

void MasterEffects::process(float* reverbBus, float* delayBus, float* output,
                            size_t frames, const Project* project) {
  if (!project || !output) return;

  reverbTailFrames_ = reverbTailFrames_ > frames ? reverbTailFrames_ - frames : 0;
  delayTailFrames_ = delayTailFrames_ > frames ? delayTailFrames_ - frames : 0;

  bool delayIdle = false;
  if (delayBus && !delayLine_.empty() && delayTailFrames_ == 0 && fxLiteEnabled()) {
    // FX lite: nothing in and the echoes have been under -120 dBFS for a whole
    // delay time, so the line only holds silence. Leave it untouched.
    float peak = 0.0f;
    for (size_t i = 0; i < frames * 2; ++i) peak = std::max(peak, fabsf(delayBus[i]));
    delayIdle = peak <= tailThreshold;
  }
  if (delayBus && !delayLine_.empty() && !delayIdle) {
    ScopedCost cost(2, frames);
    size_t capacity = delayLine_.size() / 2;
    uint8_t algorithm = project->delayAlgorithm == 1 ? 1 : 0;
    if (algorithm != activeDelayAlgorithm_) {
      resetDelay();
      activeDelayAlgorithm_ = algorithm;
    }
    float tickRate = project->tickRate > 0.0f ? project->tickRate : 50.0f;
    float feedback = std::min<int>(project->delayFeedback, 95) / 100.0f;
    float returnGain = project->perceptualEffects ? mixerGain(project->delayReturn) : project->delayReturn / 100.0f;
    float reverbSend = std::min<int>(project->delayReverbSend, 100) / 100.0f;
    if (!algorithm) {
      size_t delaySamples = (size_t)(sampleRate_ * project->delayTicks / tickRate + 0.5f);
      delaySamples = std::max<size_t>(1, std::min(delaySamples, capacity - 1));
      float filterCoefficient = lowpassCoefficient(project->delayFilterCutoffHz);
      size_t read = (delayWrite_ + capacity - delaySamples) % capacity;
      for (size_t i = 0; i < frames; ++i) {
        float wetL = lowpass(delayLine_[read * 2], filterCoefficient, delayFilterState_[0]);
        float wetR = lowpass(delayLine_[read * 2 + 1], filterCoefficient, delayFilterState_[1]);
        if (fabsf(delayBus[i * 2]) > tailThreshold || fabsf(delayBus[i * 2 + 1]) > tailThreshold ||
            fabsf(wetL) > tailThreshold || fabsf(wetR) > tailThreshold)
          delayTailFrames_ = delaySamples + frames;
        delayLine_[delayWrite_ * 2] = delayBus[i * 2] + wetR * feedback;
        delayLine_[delayWrite_ * 2 + 1] = delayBus[i * 2 + 1] + wetL * feedback;
        output[i * 2] += wetL * returnGain;
        output[i * 2 + 1] += wetR * returnGain;
        if (reverbBus) {
          reverbBus[i * 2] += wetL * reverbSend;
          reverbBus[i * 2 + 1] += wetR * reverbSend;
        }
        if (++read == capacity) read = 0;
        if (++delayWrite_ == capacity) delayWrite_ = 0;
      }
    } else {
      static constexpr uint8_t divisionTicks[] = {6, 8, 12, 18, 24, 36, 48};
      static constexpr uint8_t headMasks[] = {1, 2, 4, 3, 5, 6, 7};
      const uint8_t division = project->tapeDivision <= 6 ? project->tapeDivision : 2;
      const uint8_t heads = headMasks[project->tapeHeads <= 6 ? project->tapeHeads : 6];
      const int headCount = ((heads & 1) != 0) + ((heads & 2) != 0) + ((heads & 4) != 0);
      const float tapeFeedback = feedback / headCount;
      float baseDelay = sampleRate_ * divisionTicks[division] / tickRate;
      baseDelay = std::max(1.0f, std::min(baseDelay, ((float)capacity - 2.0f) / 3.0f));
      float tone = project->tapeTone / 255.0f;
      float toneCoefficient = lowpassCoefficient(800.0f + 19200.0f * tone * tone);
      float springControl = std::min<int>(project->tapeSpring, 100) / 100.0f;
      float springGain = springControl * (0.8f + 0.7f * springControl);
      float springFeedback = 0.58f + 0.32f * springControl * springControl;
      size_t springCapacity = springLine_.size() / 2;
      const float springDelayA = std::max(1.0f, std::min(sampleRate_ * 0.031f, (float)springCapacity - 2.0f));
      const float springDelayB = std::max(1.0f, std::min(sampleRate_ * 0.047f, (float)springCapacity - 2.0f));
      float flutterControl = std::min<int>(project->tapeFlutter, 100) / 100.0f;
      float flutterDepth = sampleRate_ * 0.020f * flutterControl * flutterControl;
      for (size_t i = 0; i < frames; ++i) {
        float flutter = sinf(flutterPhase_) * flutterDepth;
        float tapeL = 0.0f, tapeR = 0.0f;
        for (int head = 0; head < 3; ++head) if (heads & (1 << head)) {
          tapeL += delayRead(baseDelay * (head + 1) + flutter, 0);
          tapeR += delayRead(baseDelay * (head + 1) + flutter, 1);
        }
        float wetL = lowpass(tapeL, toneCoefficient, tapeFilterState_[0]);
        float wetR = lowpass(tapeR, toneCoefficient, tapeFilterState_[1]);
        float springL = 0.0f, springR = 0.0f;
        if (springCapacity) {
          size_t a = (springWrite_ + springCapacity - (size_t)springDelayA) % springCapacity;
          size_t b = (springWrite_ + springCapacity - (size_t)springDelayB) % springCapacity;
          springL = 0.5f * (springLine_[a * 2] + springLine_[b * 2]);
          springR = 0.5f * (springLine_[a * 2 + 1] + springLine_[b * 2 + 1]);
          springL = lowpass(springL, lowpassCoefficient(5500.0f), springFilterState_[0]);
          springR = lowpass(springR, lowpassCoefficient(5500.0f), springFilterState_[1]);
          springLine_[springWrite_ * 2] = delayBus[i * 2] + springR * springFeedback;
          springLine_[springWrite_ * 2 + 1] = delayBus[i * 2 + 1] + springL * springFeedback;
          if (++springWrite_ == springCapacity) springWrite_ = 0;
        }
        if (fabsf(delayBus[i * 2]) > tailThreshold || fabsf(delayBus[i * 2 + 1]) > tailThreshold ||
            fabsf(wetL) > tailThreshold || fabsf(wetR) > tailThreshold ||
            fabsf(springL) * springGain > tailThreshold || fabsf(springR) * springGain > tailThreshold)
          delayTailFrames_ = (size_t)(baseDelay * 3.0f) + frames;
        delayLine_[delayWrite_ * 2] = delayBus[i * 2] + wetL * tapeFeedback;
        delayLine_[delayWrite_ * 2 + 1] = delayBus[i * 2 + 1] + wetR * tapeFeedback;
        output[i * 2] += (wetL + springL * springGain) * returnGain;
        output[i * 2 + 1] += (wetR + springR * springGain) * returnGain;
        if (reverbBus) {
          reverbBus[i * 2] += wetL * reverbSend;
          reverbBus[i * 2 + 1] += wetR * reverbSend;
        }
        flutterPhase_ += 6.283185307179586f * 0.25f / sampleRate_;
        if (flutterPhase_ >= 6.283185307179586f) flutterPhase_ -= 6.283185307179586f;
        if (++delayWrite_ == capacity) delayWrite_ = 0;
      }
    }
  }

  if (reverbBus) {
    uint8_t algorithm = project->reverbAlgorithm == 1 ? 1 : 0;
    ScopedCost cost(algorithm, frames);
    if (algorithm != activeReverbAlgorithm_) {
      reverbTailFrames_ = 0;
      if (algorithm) {
        beadsReverb_.Init(beadsReverbMemory_, sampleRate_);
        beadsReverbHighpassInputState_[0] = beadsReverbHighpassInputState_[1] = 0.0f;
        beadsReverbHighpassOutputState_[0] = beadsReverbHighpassOutputState_[1] = 0.0f;
      } else {
        reverb_.Init(reverbMemory_, sampleRate_);
        reverbFilterState_[0] = reverbFilterState_[1] = 0.0f;
        reverbHighpassInputState_[0] = reverbHighpassInputState_[1] = 0.0f;
        reverbHighpassOutputState_[0] = reverbHighpassOutputState_[1] = 0.0f;
      }
      activeReverbAlgorithm_ = algorithm;
    }
    if (reverbTailFrames_ == 0 && fxLiteEnabled()) {
      // FX lite: no input this block and the tail has been under -120 dBFS for
      // a full second, so the reverb would only add silence. Skip it.
      float peak = 0.0f;
      for (size_t i = 0; i < frames * 2; ++i) peak = std::max(peak, fabsf(reverbBus[i]));
      if (peak <= tailThreshold) return;
    }
    float gain = project->perceptualEffects ? mixerGain(project->reverbReturn) : project->reverbReturn / 100.0f;
    if (algorithm) {
      beadsReverbFrames_.resize(frames);
      float highpassCoeff = highpassCoefficient(project->beadsReverbHighpassCutoffHz);
      for (size_t i = 0; i < frames; ++i) {
        beadsReverbFrames_[i].l = highpass(reverbBus[i * 2], highpassCoeff, beadsReverbHighpassInputState_[0], beadsReverbHighpassOutputState_[0]);
        beadsReverbFrames_[i].r = highpass(reverbBus[i * 2 + 1], highpassCoeff, beadsReverbHighpassInputState_[1], beadsReverbHighpassOutputState_[1]);
      }
      float time = project->beadsReverbTime / 255.0f;
      beadsReverb_.set_mix(1.0f);
      beadsReverb_.set_time(0.3f + 0.6f * time * time * time);
      beadsReverb_.set_lp(project->beadsReverbTone / 255.0f);
      beadsReverb_.Process(beadsReverbFrames_.data(), frames);
      for (size_t i = 0; i < frames; ++i) {
        output[i * 2] += beadsReverbFrames_[i].l * gain;
        output[i * 2 + 1] += beadsReverbFrames_[i].r * gain;
        if (fabsf(beadsReverbFrames_[i].l) > tailThreshold || fabsf(beadsReverbFrames_[i].r) > tailThreshold) reverbTailFrames_ = (size_t)sampleRate_;
      }
      return;
    }
    reverbFrames_.resize(frames);
    float filterCoefficient = lowpassCoefficient(project->reverbFilterCutoffHz);
    float highpassCoeff = highpassCoefficient(project->reverbHighpassCutoffHz);
    for (size_t i = 0; i < frames; ++i) {
      float left = highpass(reverbBus[i * 2], highpassCoeff, reverbHighpassInputState_[0], reverbHighpassOutputState_[0]);
      float right = highpass(reverbBus[i * 2 + 1], highpassCoeff, reverbHighpassInputState_[1], reverbHighpassOutputState_[1]);
      reverbFrames_[i].l = lowpass(left, filterCoefficient, reverbFilterState_[0]);
      reverbFrames_[i].r = lowpass(right, filterCoefficient, reverbFilterState_[1]);
      if (fabsf(reverbBus[i * 2]) > tailThreshold || fabsf(reverbBus[i * 2 + 1]) > tailThreshold)
        reverbTailFrames_ = (size_t)sampleRate_;
    }
    reverb_.set_amount(1.0f);
    reverb_.set_input_gain(0.2f);
    float time = project->reverbTime / 255.0f;
    reverb_.set_time(project->perceptualEffects
      ? 0.20f * powf(0.98f / 0.20f, time)
      : 0.2f + time * 0.78f);
    reverb_.set_diffusion(0.625f);
    float damping = project->reverbDamping / 255.0f;
    reverb_.set_lp(project->perceptualEffects
      ? 0.02f + 0.96f * damping * damping
      : 0.02f + damping * 0.96f);
    reverb_.Process(reverbFrames_.data(), frames);
    for (size_t i = 0; i < frames; ++i) {
      output[i * 2] += reverbFrames_[i].l * gain;
      output[i * 2 + 1] += reverbFrames_[i].r * gain;
      if (fabsf(reverbFrames_[i].l) > tailThreshold || fabsf(reverbFrames_[i].r) > tailThreshold)
        reverbTailFrames_ = (size_t)sampleRate_;
    }
  }

}
