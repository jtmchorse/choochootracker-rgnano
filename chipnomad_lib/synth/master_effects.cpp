#include "master_effects.h"

#include <algorithm>
#include <math.h>

#include "project.h"

static constexpr float tailThreshold = 0.000001f;

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
  reset();
}

void MasterEffects::reserve(size_t frames) {
  reverbFrames_.reserve(frames);
  beadsReverbFrames_.reserve(frames);
}

void MasterEffects::reset() {
  reverb_.Init(reverbMemory_, sampleRate_);
  beadsReverb_.Init(beadsReverbMemory_, sampleRate_);
  std::fill(delayLine_.begin(), delayLine_.end(), 0.0f);
  delayWrite_ = 0;
  reverbFilterState_[0] = reverbFilterState_[1] = 0.0f;
  reverbHighpassInputState_[0] = reverbHighpassInputState_[1] = 0.0f;
  reverbHighpassOutputState_[0] = reverbHighpassOutputState_[1] = 0.0f;
  beadsReverbHighpassInputState_[0] = beadsReverbHighpassInputState_[1] = 0.0f;
  beadsReverbHighpassOutputState_[0] = beadsReverbHighpassOutputState_[1] = 0.0f;
  delayFilterState_[0] = delayFilterState_[1] = 0.0f;
  reverbTailFrames_ = delayTailFrames_ = 0;
  activeReverbAlgorithm_ = 0xff;
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

void MasterEffects::process(float* reverbBus, float* delayBus, float* output,
                            size_t frames, const Project* project) {
  if (!project || !output) return;

  reverbTailFrames_ = reverbTailFrames_ > frames ? reverbTailFrames_ - frames : 0;
  delayTailFrames_ = delayTailFrames_ > frames ? delayTailFrames_ - frames : 0;

  if (delayBus && !delayLine_.empty()) {
    size_t capacity = delayLine_.size() / 2;
    float tickRate = project->tickRate > 0.0f ? project->tickRate : 50.0f;
    size_t delaySamples = (size_t)(sampleRate_ * project->delayTicks / tickRate + 0.5f);
    delaySamples = std::max<size_t>(1, std::min(delaySamples, capacity - 1));
    float feedback = std::min<int>(project->delayFeedback, 95) / 100.0f;
    float returnGain = project->perceptualEffects ? mixerGain(project->delayReturn) : project->delayReturn / 100.0f;
    float reverbSend = std::min<int>(project->delayReverbSend, 100) / 100.0f;
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
  }

  if (reverbBus) {
    uint8_t algorithm = project->reverbAlgorithm == 1 ? 1 : 0;
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
