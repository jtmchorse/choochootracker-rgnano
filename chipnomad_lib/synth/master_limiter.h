#pragma once
// Master bus peak limiter (added for the RG Nano, where a busy mix clipped the
// S16 conversion on every bass hit).
//
// Stereo-linked look-ahead brick-wall limiter:
//   - detector: max(|L|,|R|) per frame, so the image never shifts;
//   - gain need g[n] = ceiling / peak when the peak is over the ceiling, else 1;
//   - attack: sliding minimum of g over W frames followed by a W-frame
//     moving average. That pair guarantees the applied gain is <= g[j] for the
//     frame j that is leaving the delay line, with a smooth W-frame ramp, so
//     nothing needs a fast (distorting) attack;
//   - release: between the two stages, an instant-attack one-pole release
//     (~80 ms) on the linear gain, never allowed above the sliding minimum;
//   - safety: anything that still lands above the ceiling (float rounding, a
//     NaN-free but huge input) is folded into the headroom between ceiling and
//     full scale with a rational soft clip, so the S16 stage never hard-clips.
//
// Fixed storage only: no allocation, ever. Scalar and branch-light. When the
// input never exceeds the ceiling the gain is exactly 1.0f and the output is
// the input delayed by latency() frames, bit for bit.
//
// Threading: process()/reset()/init() belong to the audio thread. The stats
// counters are atomics so another thread (the RG Nano astat logger) can read
// and clear them with takeStats().
#include <atomic>
#include <math.h>
#include <stdint.h>

class MasterLimiter {
public:
  static constexpr int kMaxWindow = 256;            // frames; 1.5 ms up to 170 kHz
  static constexpr float kCeilingDb = -0.3f;
  static constexpr float kLookaheadSeconds = 0.0015f;
  static constexpr float kReleaseSeconds = 0.080f;

  MasterLimiter() { init(48000.0f); }

  void init(float sampleRate) {
    if (!(sampleRate > 1000.0f)) sampleRate = 48000.0f;
    int w = (int)(sampleRate * kLookaheadSeconds + 0.5f);
    if (w < 4) w = 4;
    if (w > kMaxWindow) w = kMaxWindow;
    window_ = w;
    invWindow_ = 1.0 / (double)w;
    // No libm exp/pow here: the RG Nano SDK's musl returns expf(-0.0004f) == 1.0f
    // (and powf(10, -0.015f) off by 1%), which made the release decay exactly 1,
    // so the gain latched at its deepest reduction and never recovered.
    releaseDecay_ = (float)expSeries(-1.0 / ((double)kReleaseSeconds * (double)sampleRate));
    ceiling_ = (float)expSeries((double)kCeilingDb / 20.0 * 2.302585092994046);
    // Aim a hair under the ceiling so float rounding in the gain chain cannot
    // push a peak over it.
    target_ = ceiling_ * 0.9999f;
    headroom_ = 1.0f - ceiling_;
    reset();
  }

  void reset() {
    for (int i = 0; i < kMaxWindow; ++i) {
      delayL_[i] = delayR_[i] = 0.0f;
      envRing_[i] = 1.0f;
      dqVal_[i] = 1.0f;
      dqIdx_[i] = 0;
    }
    delayPos_ = envPos_ = 0;
    dqHead_ = dqCount_ = 0;
    frameIndex_ = 0;
    r_ = 0.0f;
    sum_ = (double)window_;
    idle_ = window_;
  }

  // exp(x) from +, * only (halve until small, Taylor, square back). Accurate to
  // ~1e-15 for the |x| < 10 used here, independent of the platform's libm.
  static double expSeries(double x) {
    int k = 0;
    while ((x > 0.125 || x < -0.125) && k < 60) { x *= 0.5; ++k; }
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 12; ++n) { term *= x / (double)n; sum += term; }
    while (k-- > 0) sum *= sum;
    return sum;
  }
  float releaseDecay() const { return releaseDecay_; }

  // Frames of delay the limiter adds.
  int latency() const { return window_ - 1; }
  float ceiling() const { return ceiling_; }

  // In place on interleaved stereo.
  void process(float* buf, int frames) {
    const int w = window_;
    const int d = w - 1;
    float minGain = 1.0f;
    uint32_t engaged = 0;
    float reductionSum = 0.0f;   // sum of (1 - gain) over this call; <= frames, exact enough in float for a few thousand frames
    // Reduction deeper than ~0.1 dB counts as "engaged" in the stats.
    const float engagedBelow = 0.98855f;
    for (int i = 0; i < frames; ++i) {
      float l = buf[2 * i], r = buf[2 * i + 1];
      if (!(l == l)) l = 0.0f;       // NaN -> silence
      if (!(r == r)) r = 0.0f;
      if (l > 1.0e6f) l = 1.0e6f; else if (l < -1.0e6f) l = -1.0e6f;   // +-inf and absurd values
      if (r > 1.0e6f) r = 1.0e6f; else if (r < -1.0e6f) r = -1.0e6f;
      float al = fabsf(l), ar = fabsf(r);
      float peak = al > ar ? al : ar;
      const float g = peak > target_ ? target_ / peak : 1.0f;

      // Sliding minimum of g over the last w frames (monotonic deque).
      while (dqCount_ > 0) {
        int back = dqHead_ + dqCount_ - 1; if (back >= kMaxWindow) back -= kMaxWindow;
        if (dqVal_[back] >= g) --dqCount_; else break;
      }
      {
        int pos = dqHead_ + dqCount_; if (pos >= kMaxWindow) pos -= kMaxWindow;
        dqVal_[pos] = g; dqIdx_[pos] = frameIndex_; ++dqCount_;
      }
      while ((uint32_t)(frameIndex_ - dqIdx_[dqHead_]) >= (uint32_t)w) {
        if (++dqHead_ >= kMaxWindow) dqHead_ = 0;
        --dqCount_;
      }
      const float m = dqVal_[dqHead_];
      ++frameIndex_;

      // Release: drop instantly to the minimum, recover one-pole (decaying the
      // reduction r = 1 - gain, which stays exact near unity where adding to the
      // gain itself would stall in float), never above the minimum.
      float rel = r_ * releaseDecay_;
      if (rel < 1.0e-5f) rel = 0.0f;                    // snap to exact unity
      const float recovered = 1.0f - rel;
      float e;
      if (m < recovered) { e = m; r_ = 1.0f - m; }
      else { e = recovered; r_ = rel; }

      // Moving average over w frames of e.
      const float old = envRing_[envPos_];
      envRing_[envPos_] = e;
      if (++envPos_ >= w) envPos_ = 0;
      float s;
      if (e == 1.0f && old == 1.0f) {
        if (idle_ < w) ++idle_;
      } else idle_ = 0;
      if (idle_ >= w) { sum_ = (double)w; s = 1.0f; }   // exact unity, no drift
      else {
        sum_ += (double)e - (double)old;
        s = (float)(sum_ * invWindow_);
        if (s > 1.0f) s = 1.0f;
      }

      // Delay line (w-1 frames), then apply the gain.
      const float dl = delayL_[delayPos_], dr = delayR_[delayPos_];
      delayL_[delayPos_] = l; delayR_[delayPos_] = r;
      if (++delayPos_ >= d) delayPos_ = 0;
      float outL = dl * s, outR = dr * s;

      // Safety soft clip (should only ever see float-rounding overshoot).
      float a = fabsf(outL);
      if (a > ceiling_) { float x = (a - ceiling_) / headroom_; a = ceiling_ + headroom_ * x / (1.0f + x); outL = outL < 0 ? -a : a; }
      a = fabsf(outR);
      if (a > ceiling_) { float x = (a - ceiling_) / headroom_; a = ceiling_ + headroom_ * x / (1.0f + x); outR = outR < 0 ? -a : a; }
      buf[2 * i] = outL; buf[2 * i + 1] = outR;

      reductionSum += 1.0f - s;
      if (s < minGain) minGain = s;
      if (s < engagedBelow) ++engaged;
    }
    if (minGain < 1.0f) {
      float cur = statMinGain_.load(std::memory_order_relaxed);
      while (minGain < cur && !statMinGain_.compare_exchange_weak(cur, minGain, std::memory_order_relaxed)) {}
    }
    statReductionQ16_.fetch_add((uint64_t)((double)reductionSum * 65536.0), std::memory_order_relaxed);
    statEngaged_.fetch_add(engaged, std::memory_order_relaxed);
    statFrames_.fetch_add((uint32_t)frames, std::memory_order_relaxed);
  }

  // meanReduction: average of (1 - gain) over all frames (0 = never reduced).
  struct Stats { float minGain; float meanReduction; uint32_t engagedFrames; uint32_t totalFrames; };
  // Read and clear the counters since the previous call.
  Stats takeStats() {
    Stats s;
    s.minGain = statMinGain_.exchange(1.0f, std::memory_order_relaxed);
    s.engagedFrames = statEngaged_.exchange(0, std::memory_order_relaxed);
    s.totalFrames = statFrames_.exchange(0, std::memory_order_relaxed);
    const uint64_t q = statReductionQ16_.exchange(0, std::memory_order_relaxed);
    s.meanReduction = s.totalFrames ? (float)((double)q / 65536.0 / (double)s.totalFrames) : 0.0f;
    return s;
  }

private:
  int window_ = 72;
  double invWindow_ = 1.0 / 72.0;
  float releaseDecay_ = 0.999f, ceiling_ = 0.966f, target_ = 0.966f, headroom_ = 0.034f;
  float delayL_[kMaxWindow], delayR_[kMaxWindow];
  float envRing_[kMaxWindow];
  float dqVal_[kMaxWindow];
  uint32_t dqIdx_[kMaxWindow];
  int delayPos_ = 0, envPos_ = 0, dqHead_ = 0, dqCount_ = 0, idle_ = 0;
  uint32_t frameIndex_ = 0;
  float r_ = 0.0f;   // current reduction, 1 - release-stage gain
  double sum_ = 72.0;
  std::atomic<float> statMinGain_{1.0f};
  std::atomic<uint32_t> statEngaged_{0}, statFrames_{0};
  std::atomic<uint64_t> statReductionQ16_{0};
};
