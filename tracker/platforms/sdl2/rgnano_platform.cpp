#ifdef RGNANO_BUILD

#include "rgnano_platform.h"
#include "version.h"
#include "corelib_file.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fb.h>

// ---------------------------------------------------------------------------
// Data directory and log
// ---------------------------------------------------------------------------

static char dataDir[512];
static FILE* logFile = NULL;
static struct timespec startTime;

static long long nowUs(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

const char* rgnanoDataDir(void) {
  // One definition of the workspace: corelib_file.cpp (env override, SD card
  // probe, /tmp fallback).
  if (!dataDir[0] && fileGetDefaultDirectory(dataDir, sizeof(dataDir)) != 0)
    snprintf(dataDir, sizeof(dataDir), "/tmp/ChooChooTracker");
  return dataDir;
}

void rgnanoLog(const char* format, ...) {
  char line[768];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (!logFile) {
    char path[600], previous[600];
    snprintf(path, sizeof(path), "%s/rgnano-log.txt", rgnanoDataDir());
    snprintf(previous, sizeof(previous), "%s/rgnano-log.prev.txt", rgnanoDataDir());
    rename(path, previous);
    logFile = fopen(path, "w");
    clock_gettime(CLOCK_MONOTONIC, &startTime);
  }
  long long ms = nowUs() / 1000 - ((long long)startTime.tv_sec * 1000 + startTime.tv_nsec / 1000000);
  if (logFile) {
    fprintf(logFile, "[%6lld ms] %s\n", ms, line);
    fflush(logFile);
  }
  fprintf(stderr, "[%6lld ms] %s\n", ms, line);
}

// ---------------------------------------------------------------------------
// First-run asset seeding (OPK mount is read-only; user content must be
// writable and visible on the SD card)
// ---------------------------------------------------------------------------

static int copyFile(const char* from, const char* to) {
  FILE* in = fopen(from, "rb");
  if (!in) return 1;
  FILE* out = fopen(to, "wb");
  if (!out) { fclose(in); return 1; }
  static char buffer[64 * 1024];
  size_t n;
  int failed = 0;
  while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0)
    if (fwrite(buffer, 1, n, out) != n) { failed = 1; break; }
  fclose(in);
  if (fclose(out) != 0) failed = 1;
  return failed;
}

// Copies files that do not exist yet; never overwrites (user edits survive).
static int seedTree(const char* from, const char* to, int* copied) {
  DIR* dir = opendir(from);
  if (!dir) return 0;
  mkdir(to, 0755);
  int failures = 0;
  while (struct dirent* entry = readdir(dir)) {
    if (entry->d_name[0] == '.') continue;
    char source[1024], target[1024];
    snprintf(source, sizeof(source), "%s/%s", from, entry->d_name);
    snprintf(target, sizeof(target), "%s/%s", to, entry->d_name);
    struct stat st;
    if (stat(source, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      failures += seedTree(source, target, copied);
    } else if (access(target, F_OK) != 0) {
      if (copyFile(source, target) != 0) { failures++; unlink(target); }
      else (*copied)++;
    }
  }
  closedir(dir);
  return failures;
}

static void seedAssets(void) {
  // Content the user may edit or extend. title/ and licenses/ stay in the OPK.
  static const char* const folders[] = {"projects", "samples", "instruments",
    "AY_wavetables", "SR_wavetables", "waveforms", "pitch-tables", "themes", "fonts"};
  char stampPath[600], stamp[160], current[160];
  snprintf(stampPath, sizeof(stampPath), "%s/.seeded", rgnanoDataDir());
  snprintf(stamp, sizeof(stamp), "%s %s", appVersion, appBuild);
  current[0] = 0;
  if (FILE* f = fopen(stampPath, "r")) {
    if (fgets(current, sizeof(current), f)) current[strcspn(current, "\n")] = 0;
    fclose(f);
  }
  if (strcmp(current, stamp) == 0) {
    rgnanoLog("seed: up to date (%s)", stamp);
    return;
  }
  long long started = nowUs();
  int copied = 0, failures = 0;
  for (const char* folder : folders) {
    char target[600];
    snprintf(target, sizeof(target), "%s/%s", rgnanoDataDir(), folder);
    failures += seedTree(folder, target, &copied);
  }
  rgnanoLog("seed: copied %d new files, %d failures, %lld ms", copied, failures, (nowUs() - started) / 1000);
  if (!failures) {
    if (FILE* f = fopen(stampPath, "w")) { fprintf(f, "%s\n", stamp); fclose(f); }
  }
}

// ---------------------------------------------------------------------------
// Audio: open first, in a worker thread, with a watchdog
// ---------------------------------------------------------------------------

// Round 3 on the device saw SDL_OpenAudioDevice(default) block forever, past
// an alarm(): the thread was stuck in the kernel. So the open runs in a worker;
// if it has not finished in kOpenTimeoutMs the thread is abandoned (detached)
// and the speaker is opened explicitly by its SDL device name instead.
static const int kOpenTimeoutMs = 3000;

static std::atomic<AudioCallback*> audioCallbackPtr{nullptr};
static std::atomic<int> audioPaused{1};
static SDL_AudioDeviceID audioDevice = 0;
static int audioRate = 0, audioSamples = 0;
static std::string audioDeviceName;       // name that worked ("" = default)
static bool audioFailed = false;          // nothing could be opened
static std::thread sinkThread;
static std::atomic<bool> sinkStop{false};

static void enableFlushToZero(void) {
#if defined(__arm__)
  // Denormals trap to support code on the A7's VFP: the engine budget report
  // measured 2x on Plaits and 10x on the reverb/OTT sends without this.
  thread_local bool done = false;
  if (done) return;
  uint32_t fpscr;
  __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
  fpscr |= (1u << 24);
  __asm__ volatile("vmsr fpscr, %0" : : "r"(fpscr));
  done = true;
#endif
}

static void SDLCALL audioTrampoline(void*, Uint8* stream, int length) {
  enableFlushToZero();
  AudioCallback* callback = audioCallbackPtr.load(std::memory_order_acquire);
  if (callback) callback((int16_t*)stream, length / 4);
  else memset(stream, 0, length);
}

struct OpenAttempt {
  std::mutex mutex;
  std::condition_variable done;
  bool finished = false;
  SDL_AudioDeviceID device = 0;
  SDL_AudioSpec obtained;
  std::string error;
  std::string speakerName;
};

static bool isSpeaker(const char* name) {
  // 'V3s Audio Codec, ' on the RG Nano; the USB DAC is 'JadeAudio JA11, USB Audio'.
  return name && (strstr(name, "V3s") || strstr(name, "Codec")) && !strstr(name, "USB");
}

static void openWorker(std::shared_ptr<OpenAttempt> attempt, std::string name, bool useName,
                       bool speaker, int rate, int samples) {
  if (getenv("RGNANO_AUDIO_TEST_HANG") && !useName && !speaker) {
    // Test hook: emulate the round-3 hang in the default open (watchdog test).
    rgnanoLog("audio worker: RGNANO_AUDIO_TEST_HANG set, blocking forever");
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
  }
  rgnanoLog("audio worker: SDL_InitSubSystem(AUDIO)");
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    std::lock_guard<std::mutex> lock(attempt->mutex);
    attempt->error = SDL_GetError();
    attempt->finished = true;
    attempt->done.notify_all();
    return;
  }
  rgnanoLog("audio worker: driver=%s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
  int count = SDL_GetNumAudioDevices(0);
  std::string speakerName;
  for (int i = 0; i < count; i++) {
    const char* n = SDL_GetAudioDeviceName(i, 0);
    rgnanoLog("audio worker: output device %d: '%s'", i, n ? n : "(null)");
    if (speakerName.empty() && isSpeaker(n)) speakerName = n;
  }
  if (speaker) {
    if (speakerName.empty()) {
      std::lock_guard<std::mutex> lock(attempt->mutex);
      attempt->error = "no speaker device in the SDL list";
      attempt->finished = true;
      attempt->done.notify_all();
      return;
    }
    name = speakerName;
    useName = true;
  }
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = rate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = (Uint16)samples;
  want.callback = audioTrampoline;
  rgnanoLog("audio worker: SDL_OpenAudioDevice(%s%s%s) %d Hz S16 2ch %d",
            useName ? "'" : "", useName ? name.c_str() : "default", useName ? "'" : "", rate, samples);
  SDL_zero(have);
  // allowed_changes = 0: SDL converts to whatever the device really runs, so
  // the engine always renders the rate it was initialised with.
  SDL_AudioDeviceID device = SDL_OpenAudioDevice(useName ? name.c_str() : NULL, 0, &want, &have, 0);
  std::lock_guard<std::mutex> lock(attempt->mutex);
  attempt->device = device;
  attempt->obtained = have;
  attempt->speakerName = speakerName;
  if (!device) attempt->error = SDL_GetError();
  attempt->finished = true;
  attempt->done.notify_all();
}

static SDL_AudioDeviceID tryOpen(const char* label, const std::string& name, bool useName, bool speaker,
                                 int rate, int samples, std::string* usedName) {
  rgnanoLog("audio: attempt '%s' (timeout %d ms)", label, kOpenTimeoutMs);
  auto attempt = std::make_shared<OpenAttempt>();
  long long started = nowUs();
  std::thread(openWorker, attempt, name, useName, speaker, rate, samples).detach();
  std::unique_lock<std::mutex> lock(attempt->mutex);
  if (!attempt->done.wait_for(lock, std::chrono::milliseconds(kOpenTimeoutMs), [&] { return attempt->finished; })) {
    rgnanoLog("audio: attempt '%s' TIMED OUT after %d ms, abandoning the worker thread", label, kOpenTimeoutMs);
    return 0;
  }
  if (!attempt->device) {
    rgnanoLog("audio: attempt '%s' failed after %lld ms: %s", label, (nowUs() - started) / 1000, attempt->error.c_str());
    return 0;
  }
  const SDL_AudioSpec& have = attempt->obtained;
  rgnanoLog("audio: attempt '%s' OK in %lld ms: device=%u driver=%s freq=%d format=0x%x ch=%d samples=%d bytes=%u",
            label, (nowUs() - started) / 1000, attempt->device,
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
            have.freq, have.format, have.channels, have.samples, have.size);
  if (usedName) *usedName = speaker ? attempt->speakerName : name;
  return attempt->device;
}

static bool openAudio(int rate, int samples) {
  const char* forced = getenv("RGNANO_AUDIO_DEVICE");
  std::string used;
  SDL_AudioDeviceID device = 0;
  if (forced && forced[0]) {
    device = tryOpen("forced", forced, true, false, rate, samples, &used);
  } else if (!audioDeviceName.empty()) {
    device = tryOpen("remembered", audioDeviceName, true, false, rate, samples, &used);
  } else {
    device = tryOpen("default", "", false, false, rate, samples, &used);
    if (!device) device = tryOpen("speaker", "", false, true, rate, samples, &used);
  }
  if (!device) {
    rgnanoLog("audio: NO DEVICE. Running silent with a software clock");
    audioFailed = true;
    audioRate = rate;
    audioSamples = samples;
    return false;
  }
  audioFailed = false;
  audioDevice = device;
  audioRate = rate;
  audioSamples = samples;
  audioDeviceName = used;
  return true;
}

static void stopSink(void) {
  if (!sinkThread.joinable()) return;
  sinkStop = true;
  sinkThread.join();
}

// No audio hardware: still clock the engine so playback, the sequencer and
// the UI behave. Output is discarded.
static void startSink(int rate, int samples) {
  stopSink();
  sinkStop = false;
  sinkThread = std::thread([rate, samples] {
    std::vector<int16_t> scratch((size_t)samples * 2);
    auto next = std::chrono::steady_clock::now();
    const auto period = std::chrono::microseconds((long long)samples * 1000000LL / rate);
    while (!sinkStop) {
      next += period;
      if (!audioPaused.load()) {
        enableFlushToZero();
        AudioCallback* callback = audioCallbackPtr.load(std::memory_order_acquire);
        if (callback) callback(scratch.data(), samples);
      }
      std::this_thread::sleep_until(next);
    }
  });
}

int rgnanoAudioAttach(AudioCallback* callback, int sampleRate, int bufferSize) {
  if ((audioDevice || audioFailed) && audioRate == sampleRate && audioSamples == bufferSize) {
    audioCallbackPtr.store(callback, std::memory_order_release);
    if (audioDevice) {
      rgnanoLog("audio: attach callback to the early-opened device (%d Hz, %d)", sampleRate, bufferSize);
    } else {
      // The early open already failed with these parameters; do not burn
      // another 6 s of watchdog timeouts at boot.
      rgnanoLog("audio: early open had failed, attaching callback to the software clock");
      if (!sinkThread.joinable()) startSink(sampleRate, bufferSize);
    }
    return 0;
  }
  rgnanoLog("audio: (re)configure %d Hz, %d frames", sampleRate, bufferSize);
  rgnanoAudioClose();
  if (!openAudio(sampleRate, bufferSize)) {
    audioCallbackPtr.store(callback, std::memory_order_release);
    audioRate = sampleRate;
    audioSamples = bufferSize;
    startSink(sampleRate, bufferSize);
    return 0;
  }
  audioCallbackPtr.store(callback, std::memory_order_release);
  return 0;
}

void rgnanoAudioPause(int isPaused) {
  audioPaused = isPaused;
  if (audioDevice) SDL_PauseAudioDevice(audioDevice, isPaused);
}

void rgnanoAudioClose(void) {
  stopSink();
  audioCallbackPtr.store(nullptr, std::memory_order_release);
  if (audioDevice) {
    SDL_PauseAudioDevice(audioDevice, 1);
    SDL_CloseAudioDevice(audioDevice);
    audioDevice = 0;
  }
}

// ---------------------------------------------------------------------------
// Early init
// ---------------------------------------------------------------------------

void rgnanoPlatformEarlyInit(void) {
  rgnanoDataDir();
  rgnanoLog("ChooChooTracker %s (%s) RGNANO_BUILD, data dir %s", appVersion, appBuild, rgnanoDataDir());
  {
    char cwd[512];
    rgnanoLog("cwd=%s SDL_VIDEODRIVER=%s SDL_EVDEV_DEVICES=%s SDL_AUDIODRIVER=%s",
      getcwd(cwd, sizeof(cwd)) ? cwd : "?",
      getenv("SDL_VIDEODRIVER") ? getenv("SDL_VIDEODRIVER") : "(unset)",
      getenv("SDL_EVDEV_DEVICES") ? getenv("SDL_EVDEV_DEVICES") : "(unset)",
      getenv("SDL_AUDIODRIVER") ? getenv("SDL_AUDIODRIVER") : "(unset)");
  }
  // Audio before anything else touches the tty or the evdev nodes: that is the
  // order round 2 used when the open worked.
  rgnanoLog("early init: opening audio first (32000 Hz, 512 frames)");
  openAudio(32000, 512);
  seedAssets();
}

// ---------------------------------------------------------------------------
// Presenter: area-downscale to 240x240, RGB565, /dev/fb0 or a dump file
// ---------------------------------------------------------------------------

void rgnanoCanvasSize(int* width, int* height) {
  int w = 480, h = 320;
  const char* env = getenv("RGNANO_CANVAS");
  int ew, eh;
  if (env && sscanf(env, "%dx%d", &ew, &eh) == 2 && ew >= 64 && eh >= 64 && ew <= 1280 && eh <= 960) {
    w = ew;
    h = eh;
  }
  if (width) *width = w;
  if (height) *height = h;
}

namespace {
struct Axis {
  int source = 0;
  std::vector<int> first;     // first source index per destination index
  std::vector<int> count;
  std::vector<int> offset;    // into weights
  std::vector<uint16_t> weights;
  void build(int src, int dst) {
    source = src;
    first.assign(dst, 0);
    count.assign(dst, 0);
    offset.assign(dst, 0);
    weights.clear();
    for (int i = 0; i < dst; i++) {
      long long lo = (long long)i * src, hi = (long long)(i + 1) * src;
      int j0 = (int)(lo / dst), j1 = (int)((hi - 1) / dst);
      first[i] = j0;
      count[i] = j1 - j0 + 1;
      offset[i] = (int)weights.size();
      for (int j = j0; j <= j1; j++) {
        long long a = (long long)j * dst, b = a + dst;
        weights.push_back((uint16_t)((hi < b ? hi : b) - (lo > a ? lo : a)));
      }
    }
  }
};

Axis axisX, axisY;
int tableW = 0, tableH = 0;
uint16_t frame565[RGNANO_PHYSICAL_W * RGNANO_PHYSICAL_H];
bool haveFrame = false;
uint8_t* fbMemory = NULL;
size_t fbLength = 0;
int fbStride = 0, fbFd = -1;
bool fbTried = false;
const char* dumpPath = NULL;
const char* ppmPath = NULL;
int ppmSeconds = 5;
bool envRead = false;
long long lastDumpUs = 0, lastPpmUs = 0;
unsigned long frameCount = 0;
unsigned long long sumScaleUs = 0, sumBlitUs = 0, sumReadUs = 0;
unsigned maxScaleUs = 0, maxBlitUs = 0, maxReadUs = 0;
}

static void openFramebuffer(void) {
  fbTried = true;
  const char* path = getenv("RGNANO_FB_DEV");
  if (!path || !path[0]) path = "/dev/fb0";
  fbFd = open(path, O_RDWR);
  if (fbFd < 0) { rgnanoLog("fb: open %s failed: %s", path, strerror(errno)); return; }
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  if (ioctl(fbFd, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fbFd, FBIOGET_FSCREENINFO, &fix) != 0) {
    rgnanoLog("fb: ioctl failed: %s", strerror(errno));
    close(fbFd); fbFd = -1; return;
  }
  rgnanoLog("fb: id='%s' %ux%u virtual %ux%u bpp=%u stride=%u yoffset=%u smem_len=%u",
            fix.id, var.xres, var.yres, var.xres_virtual, var.yres_virtual, var.bits_per_pixel,
            fix.line_length, var.yoffset, fix.smem_len);
  if (var.bits_per_pixel != 16 || var.xres != RGNANO_PHYSICAL_W || var.yres != RGNANO_PHYSICAL_H) {
    rgnanoLog("fb: unexpected geometry, not drawing to it");
    close(fbFd); fbFd = -1; return;
  }
  fbStride = (int)fix.line_length;
  fbLength = fix.smem_len;
  fbMemory = (uint8_t*)mmap(NULL, fbLength, PROT_READ | PROT_WRITE, MAP_SHARED, fbFd, 0);
  if (fbMemory == MAP_FAILED) {
    rgnanoLog("fb: mmap failed: %s", strerror(errno));
    fbMemory = NULL; close(fbFd); fbFd = -1;
  } else {
    rgnanoLog("fb: mapped %zu bytes", fbLength);
  }
}

static void writeFileAtomic(const char* path, const void* data, size_t size) {
  char tmp[700];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE* f = fopen(tmp, "wb");
  if (!f) return;
  fwrite(data, 1, size, f);
  fclose(f);
  rename(tmp, path);
}

static void writePpm(const char* path) {
  std::vector<uint8_t> out;
  const int n = RGNANO_PHYSICAL_W * RGNANO_PHYSICAL_H;
  char header[32];
  int hl = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", RGNANO_PHYSICAL_W, RGNANO_PHYSICAL_H);
  out.assign(header, header + hl);
  out.reserve(hl + n * 3);
  for (int i = 0; i < n; i++) {
    uint16_t p = frame565[i];
    uint8_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    out.push_back((uint8_t)((r << 3) | (r >> 2)));
    out.push_back((uint8_t)((g << 2) | (g >> 4)));
    out.push_back((uint8_t)((b << 3) | (b >> 2)));
  }
  writeFileAtomic(path, out.data(), out.size());
}

static void flushDumps(bool force) {
  long long now = nowUs();
  if (dumpPath && (force || now - lastDumpUs >= 100000)) {
    writeFileAtomic(dumpPath, frame565, sizeof(frame565));
    lastDumpUs = now;
  }
  if (ppmPath && (force || now - lastPpmUs >= (long long)ppmSeconds * 1000000LL)) {
    writePpm(ppmPath);
    lastPpmUs = now;
  }
}

void rgnanoPresent(const uint32_t* argb, int w, int h, unsigned readbackUs) {
  if (!envRead) {
    envRead = true;
    dumpPath = getenv("RGNANO_FB_DUMP");
    if (dumpPath && !dumpPath[0]) dumpPath = NULL;
    ppmPath = getenv("RGNANO_FB_PPM");
    if (ppmPath && !ppmPath[0]) ppmPath = NULL;
    if (const char* s = getenv("RGNANO_FB_PPM_SECS")) { int v = atoi(s); if (v > 0) ppmSeconds = v; }
    rgnanoLog("present: canvas %dx%d -> %dx%d, fb dump=%s ppm=%s", w, h, RGNANO_PHYSICAL_W, RGNANO_PHYSICAL_H,
              dumpPath ? dumpPath : "(off)", ppmPath ? ppmPath : "(off)");
  }
  if (w != tableW || h != tableH) {
    axisX.build(w, RGNANO_PHYSICAL_W);
    axisY.build(h, RGNANO_PHYSICAL_H);
    tableW = w;
    tableH = h;
  }
  long long t0 = nowUs();

  // Separable box (area) filter. Every destination pixel covers an exact
  // fractional source area: weights per axis sum to the source length.
  static std::vector<uint32_t> hrow, acc;
  hrow.resize((size_t)RGNANO_PHYSICAL_W * 3);
  acc.resize((size_t)RGNANO_PHYSICAL_W * 3);
  const uint64_t divisor = (uint64_t)w * (uint64_t)h;
  const uint64_t reciprocal = (1ULL << 32) / divisor;
  int cachedRow = -1;
  for (int dy = 0; dy < RGNANO_PHYSICAL_H; dy++) {
    std::fill(acc.begin(), acc.end(), 0u);
    for (int k = 0; k < axisY.count[dy]; k++) {
      const int sy = axisY.first[dy] + k;
      const uint32_t wy = axisY.weights[axisY.offset[dy] + k];
      if (sy != cachedRow) {
        const uint32_t* line = argb + (size_t)sy * w;
        for (int dx = 0; dx < RGNANO_PHYSICAL_W; dx++) {
          uint32_t r = 0, g = 0, b = 0;
          const uint16_t* wx = &axisX.weights[axisX.offset[dx]];
          const uint32_t* px = line + axisX.first[dx];
          for (int j = axisX.count[dx]; j > 0; j--, px++, wx++) {
            const uint32_t p = *px, wt = *wx;
            r += ((p >> 16) & 0xff) * wt;
            g += ((p >> 8) & 0xff) * wt;
            b += (p & 0xff) * wt;
          }
          hrow[dx * 3] = r; hrow[dx * 3 + 1] = g; hrow[dx * 3 + 2] = b;
        }
        cachedRow = sy;
      }
      for (int i = 0; i < RGNANO_PHYSICAL_W * 3; i++) acc[i] += hrow[i] * wy;
    }
    uint16_t* out = frame565 + dy * RGNANO_PHYSICAL_W;
    for (int dx = 0; dx < RGNANO_PHYSICAL_W; dx++) {
      uint32_t r = (uint32_t)(((uint64_t)acc[dx * 3] * reciprocal + (1ULL << 31)) >> 32);
      uint32_t g = (uint32_t)(((uint64_t)acc[dx * 3 + 1] * reciprocal + (1ULL << 31)) >> 32);
      uint32_t b = (uint32_t)(((uint64_t)acc[dx * 3 + 2] * reciprocal + (1ULL << 31)) >> 32);
      if (r > 255) r = 255;
      if (g > 255) g = 255;
      if (b > 255) b = 255;
      out[dx] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
  haveFrame = true;
  long long t1 = nowUs();

  if (dumpPath || ppmPath) {
    flushDumps(false);
  } else {
    if (!fbTried) openFramebuffer();
    if (fbMemory) {
      if (fbStride == RGNANO_PHYSICAL_W * 2) {
        memcpy(fbMemory, frame565, sizeof(frame565));
      } else {
        for (int y = 0; y < RGNANO_PHYSICAL_H; y++)
          memcpy(fbMemory + (size_t)y * fbStride, frame565 + y * RGNANO_PHYSICAL_W, RGNANO_PHYSICAL_W * 2);
      }
    }
  }
  long long t2 = nowUs();

  unsigned scaleUs = (unsigned)(t1 - t0), blitUs = (unsigned)(t2 - t1);
  sumScaleUs += scaleUs; sumBlitUs += blitUs; sumReadUs += readbackUs;
  if (scaleUs > maxScaleUs) maxScaleUs = scaleUs;
  if (blitUs > maxBlitUs) maxBlitUs = blitUs;
  if (readbackUs > maxReadUs) maxReadUs = readbackUs;
  if (++frameCount % 300 == 0) {
    rgnanoLog("present: %lu frames, avg us read=%llu scale=%llu blit=%llu, max read=%u scale=%u blit=%u",
              frameCount, sumReadUs / 300, sumScaleUs / 300, sumBlitUs / 300, maxReadUs, maxScaleUs, maxBlitUs);
    sumScaleUs = sumBlitUs = sumReadUs = 0;
    maxScaleUs = maxBlitUs = maxReadUs = 0;
  }
  if (frameCount == 1 || frameCount == 10)
    rgnanoLog("present: frame %lu read=%u us scale=%u us blit=%u us", frameCount, readbackUs, scaleUs, blitUs);
}

void rgnanoPlatformShutdown(void) {
  rgnanoAudioClose();
  if (haveFrame) flushDumps(true);
  rgnanoLog("shutdown");
}

#endif // RGNANO_BUILD
