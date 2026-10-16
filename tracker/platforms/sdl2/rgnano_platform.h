#ifndef RGNANO_PLATFORM_H
#define RGNANO_PLATFORM_H

// Anbernic RG Nano (DrUm78 FunKey-OS) platform glue: data directory, log,
// first-run asset seeding, ALSA open with a watchdog, and the 240x240 RGB565
// framebuffer presenter. Everything here is compiled only for RGNANO_BUILD.

#include <stdint.h>
#include "corelib_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Writable workspace (default /mnt/ChooChooTracker, override RGNANO_DATA_DIR).
const char* rgnanoDataDir(void);

// Appends a timestamped line to <data dir>/rgnano-log.txt (and stderr).
void rgnanoLog(const char* format, ...) __attribute__((format(printf, 1, 2)));

// First call in main(): creates the data dir + log, seeds bundled content on
// first run, then opens audio BEFORE video/evdev/tty are touched.
void rgnanoPlatformEarlyInit(void);
void rgnanoPlatformShutdown(void);

// Logical canvas the UI renders into (default 480x320, override with
// RGNANO_CANVAS=WxH). The presenter area-downscales it to 240x240.
void rgnanoCanvasSize(int* width, int* height);
#define RGNANO_PHYSICAL_W 240
#define RGNANO_PHYSICAL_H 240

// Presents a finished frame (ARGB8888, w*h, tightly packed) to /dev/fb0, or to
// the RGNANO_FB_DUMP file. readbackUs is the caller's SDL_RenderReadPixels cost.
void rgnanoPresent(const uint32_t* argb, int w, int h, unsigned readbackUs);

// CPU time (us) of the calling thread; the presenter and stats use it so a
// preempting audio thread does not inflate the numbers.
long long rgnanoThreadCpuUs(void);
// Per-UI-frame hook (main thread): audio callback / clipping / CPU window log
// every 10 s while `playing`, and a summary when playback stops.
void rgnanoStatsTick(int playing);
// Master limiter gain reduction since the last call (min gain 1.0 = none, mean of
// (1 - gain), frames processed / engaged >0.1 dB). Main thread; folded into the next astat line.
void rgnanoStatsAddLimiter(float minGain, float meanReduction, unsigned engagedFrames, unsigned totalFrames);

// Audio (called from corelib_audio.cpp).
int rgnanoAudioAttach(AudioCallback* callback, int sampleRate, int bufferSize);
void rgnanoAudioPause(int isPaused);
void rgnanoAudioClose(void);

#ifdef __cplusplus
}
#endif
#endif
