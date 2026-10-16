#ifdef RGNANO_BUILD

#include "rgnano_platform.h"
#include "rgnano_scale.h"
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
#include <signal.h>
#include <sys/wait.h>
#include <alsa/asoundlib.h>

// ---------------------------------------------------------------------------
// Data directory and log
// ---------------------------------------------------------------------------

static char dataDir[512];
static FILE* logFile = NULL;
static struct timespec startTime;
static bool logSync = true;  // fsync every line during early init and after audio lines

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
    // The tester hard-powers-off a hung device: make the lines durable.
    if (logSync || strncmp(line, "audio", 5) == 0) fsync(fileno(logFile));
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

// ---------------------------------------------------------------------------
// Out-of-process ALSA probe. Round 3/4 on the device hung inside the USB DAC
// open (kernel, uninterruptible). A forked child tries each candidate with
// alsa-lib only (no SDL, no fb); the parent waits kProbeTimeoutMs, then
// SIGKILLs and moves on without ever waiting on a D-state child again.
// ---------------------------------------------------------------------------

static const int kProbeTimeoutMs = 3000;  // alsa-lib open + SDL init/open + 150 ms of silence

struct ProbeResult {
  bool ok = false;
  bool timedOut = false;
  bool crashed = false;
  std::string detail;
};

// Child stage 2: exactly what the parent will do afterwards, in the child's own
// address space. Round 4.1 died inside SDL_OpenAudioDevice (SIGSEGV in the
// USB DAC path), which an alsa-lib-only open cannot predict. Same env
// (AUDIODEV, SDL_AUDIODRIVER=alsa), same SDL calls and flags as openWorker /
// tryOpen, then 150 ms of silence through the real callback. No video, no fb.
static std::atomic<int> probeCallbacks{0};
static void SDLCALL probeSilence(void*, Uint8* stream, int length) {
  memset(stream, 0, (size_t)length);
  probeCallbacks.fetch_add(1);
}

static const char* probeSdlStage(const char* name, int rate, int period, char* msg, size_t msgSize) {
  setenv("AUDIODEV", name, 1);
  setenv("SDL_AUDIODRIVER", "alsa", 1);
  unsetenv("SDL_AUDIO_DEVICE_NAME");
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { snprintf(msg, msgSize, "sdl init: %s", SDL_GetError()); return NULL; }
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = rate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = (Uint16)period;
  want.callback = probeSilence;
  SDL_zero(have);
  {
    // Test hook: die inside the SDL stage. "1" crashes every candidate, any
    // other value only the candidate with that ALSA name.
    const char* crash = getenv("RGNANO_AUDIO_PROBE_TEST_CRASH");
    if (crash && (strcmp(crash, "1") == 0 || strcmp(crash, name) == 0)) raise(SIGSEGV);
  }
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  if (!dev) { snprintf(msg, msgSize, "sdl open: %s", SDL_GetError()); return NULL; }
  SDL_PauseAudioDevice(dev, 0);
  SDL_Delay(150);
  SDL_PauseAudioDevice(dev, 1);
  SDL_CloseAudioDevice(dev);
  SDL_Quit();
  snprintf(msg, msgSize, "ok sdl rate=%d samples=%d callbacks=%d", have.freq, have.samples, probeCallbacks.load());
  return msg;
}

// The child reports progress on the pipe as it goes, one marker per stage
// ("A" alsa-lib open passed, "S" entering the SDL stage), and a final "+text"
// or "-text". If it dies by a signal the parent still knows the last stage.
static void probeChild(int wfd, const char* name, int rate, int period) {
  char msg[160];
  ssize_t ignored;
  if (getenv("RGNANO_AUDIO_PROBE_TEST_HANG")) for (;;) sleep(1000);
  snd_pcm_t* pcm = NULL;
  int err = snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (err < 0) { snprintf(msg, sizeof(msg), "open: %s", snd_strerror(err)); goto done; }
  {
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);
    unsigned int r = (unsigned int)rate;
    snd_pcm_uframes_t per = (snd_pcm_uframes_t)period;
    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0 ||
        (err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
        (err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
        (err = snd_pcm_hw_params_set_channels(pcm, hw, 2)) < 0 ||
        (err = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0)) < 0 ||
        (err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &per, 0)) < 0 ||
        (err = snd_pcm_hw_params(pcm, hw)) < 0) {
      snprintf(msg, sizeof(msg), "hw_params: %s", snd_strerror(err));
      goto done;
    }
    if ((int)r != rate) {
      snprintf(msg, sizeof(msg), "rate %u != %d", r, rate);
      err = -1;
      goto done;
    }
    if ((err = snd_pcm_prepare(pcm)) < 0) { snprintf(msg, sizeof(msg), "prepare: %s", snd_strerror(err)); goto done; }
    {
      // ~100 ms of silence; EAGAIN (non-blocking, buffer full) counts as written.
      std::vector<int16_t> silence((size_t)period * 2, 0);
      int chunks = rate / 10 / period + 1;
      for (int i = 0; i < chunks; i++) {
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, silence.data(), (snd_pcm_uframes_t)period);
        if (w == -EAGAIN) { usleep(5000); continue; }
        if (w == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (w < 0) { err = (int)w; snprintf(msg, sizeof(msg), "write: %s", snd_strerror(err)); goto done; }
      }
    }
    err = 0;
    snprintf(msg, sizeof(msg), "ok alsa rate=%u period=%lu", r, (unsigned long)per);
  }
done:
  if (pcm) { snd_pcm_close(pcm); pcm = NULL; }
  if (err == 0 && !getenv("RGNANO_AUDIO_PROBE_ALSA_ONLY")) {
    ignored = write(wfd, "A", 1);  // alsa-lib stage passed; the SDL stage can still crash
    ignored = write(wfd, "S", 1);
    char sdlMsg[120], both[160];
    if (probeSdlStage(name, rate, period, sdlMsg, sizeof(sdlMsg))) {
      snprintf(both, sizeof(both), "%s; %s", msg, sdlMsg);
    } else {
      err = -1;
      snprintf(both, sizeof(both), "%s", sdlMsg);
    }
    snprintf(msg, sizeof(msg), "%s", both);
  }
  char out[200];
  int n = snprintf(out, sizeof(out), "%c%s", err == 0 ? '+' : '-', msg);
  ignored = write(wfd, out, (size_t)n);
  (void)ignored;
  _exit(err == 0 ? 0 : 1);
}

static ProbeResult probeAlsa(const char* name, int rate, int period) {
  ProbeResult res;
  int fds[2];
  if (pipe(fds) != 0) { res.detail = "pipe failed"; return res; }
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  pid_t pid = fork();
  if (pid < 0) { close(fds[0]); close(fds[1]); res.detail = "fork failed"; return res; }
  if (pid == 0) {
    close(fds[0]);
    probeChild(fds[1], name, rate, period);
  }
  close(fds[1]);
  long long started = nowUs();
  int status = 0;
  bool exited = false;
  while ((nowUs() - started) / 1000 < kProbeTimeoutMs) {
    pid_t w = waitpid(pid, &status, WNOHANG);
    if (w == pid) { exited = true; break; }
    usleep(5000);
  }
  // Whatever the child managed to write, even if it was killed or crashed.
  char buf[260];
  ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
  buf[n > 0 ? n : 0] = 0;
  const char* text = buf;
  bool alsaPassed = false, inSdl = false;
  for (; *text == 'A' || *text == 'S'; text++) {
    if (*text == 'A') alsaPassed = true;
    if (*text == 'S') inSdl = true;
  }
  const char* stage = inSdl ? "in the SDL open stage (alsa-lib open had passed)"
                            : (alsaPassed ? "after the alsa-lib open" : "in the alsa-lib stage");
  if (!exited) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, WNOHANG);  // one non-blocking reap; a D-state child is abandoned
    res.timedOut = true;
    res.detail = std::string("TIMED OUT ") + stage + ", child killed and abandoned";
  } else if (WIFSIGNALED(status)) {
    int sig = WTERMSIG(status);
    const char* sigName = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGABRT ? "SIGABRT" :
                          sig == SIGILL ? "SIGILL" : sig == SIGFPE ? "SIGFPE" : sig == SIGKILL ? "SIGKILL" :
                          sig == SIGTERM ? "SIGTERM" : sig == SIGPIPE ? "SIGPIPE" : "signal";
    char d[200];
    snprintf(d, sizeof(d), "child killed by %s (%d) %s", sigName, sig, stage);
    res.crashed = true;
    res.detail = d;
  } else if (*text) {
    res.ok = text[0] == '+' && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    res.detail = text + 1;
    if (!res.ok && WIFEXITED(status)) res.detail += " (child exit code " + std::to_string(WEXITSTATUS(status)) + ")";
  } else {
    res.detail = std::string("child exited without a report, exit code ") +
                 (WIFEXITED(status) ? std::to_string(WEXITSTATUS(status)) : "?") + " " + stage;
  }
  close(fds[0]);
  return res;
}

struct ProbeWinner {
  bool valid = false;
  std::string name;   // ALSA PCM name
  int devRate = 0;    // rate the device accepted
  int reqRate = 0;    // rate the engine asked for when probing
};
static ProbeWinner probeWinner;

static bool runProbe(int rate, int period) {
  long long begun = nowUs();
  struct Cand { const char* name; int rate; };
  bool card1 = access("/proc/asound/card1", F_OK) == 0 || getenv("RGNANO_AUDIO_PROBE_FORCE_CARD1");
  rgnanoLog("audio probe: card1 %s, probing out of process (timeout %d ms each)", card1 ? "present" : "absent", kProbeTimeoutMs);
  std::vector<Cand> cands;
  if (card1) { cands.push_back({"plughw:1,0", rate}); cands.push_back({"plughw:1,0", 48000}); }
  cands.push_back({"default", rate});
  cands.push_back({"plughw:0,0", rate});
  bool card1Hung = false;
  bool dacFailed = false;  // some DAC candidate (plughw:1,0 or the default routed there) did not work
  for (const Cand& c : cands) {
    // A hung card1 open leaves a D-state child on that device; the default
    // PCM routes there when the DAC is plugged, so skip straight to the speaker.
    if (card1Hung && (strcmp(c.name, "plughw:1,0") == 0 || strcmp(c.name, "default") == 0)) {
      rgnanoLog("audio probe: skip '%s' %d Hz (card1 hung earlier)", c.name, c.rate);
      continue;
    }
    long long t0 = nowUs();
    ProbeResult r = probeAlsa(c.name, c.rate, period);
    rgnanoLog("audio probe: '%s' %d Hz -> %s in %lld ms (%s)", c.name, c.rate,
              r.ok ? "OK" : (r.timedOut ? "HANG" : (r.crashed ? "CRASH" : "fail")), (nowUs() - t0) / 1000, r.detail.c_str());
    if ((r.timedOut || r.crashed) && strcmp(c.name, "plughw:1,0") == 0) card1Hung = true;
    if (!r.ok && card1 && (strcmp(c.name, "plughw:1,0") == 0 || strcmp(c.name, "default") == 0)) dacFailed = true;
    if (r.ok) {
      if (card1 && dacFailed && strcmp(c.name, "plughw:0,0") == 0)
        rgnanoLog("audio probe: the USB DAC (card1) is UNUSABLE on this device; the built-in speaker (plughw:0,0) is in use");
      probeWinner.valid = true;
      probeWinner.name = c.name;
      probeWinner.devRate = c.rate;
      probeWinner.reqRate = rate;
      rgnanoLog("audio probe: winner '%s' at %d Hz, total probe delay %lld ms", c.name, c.rate, (nowUs() - begun) / 1000);
      return true;
    }
  }
  if (card1)
    rgnanoLog("audio probe: the USB DAC (card1) is UNUSABLE on this device and so is the built-in speaker; no audio output");
  rgnanoLog("audio probe: no candidate worked, total probe delay %lld ms", (nowUs() - begun) / 1000);
  return false;
}

// ---------------------------------------------------------------------------
// In-process SDL open of the probe winner. SDL 2.30 ALSA (SDL_alsa_audio.c,
// get_audio_device) opens the PCM named by $AUDIODEV when no device name is
// passed, so AUDIODEV=<alsa name> + SDL_OpenAudioDevice(NULL) picks the winner.
// ---------------------------------------------------------------------------

struct OpenAttempt {
  std::mutex mutex;
  std::condition_variable done;
  bool finished = false;
  SDL_AudioDeviceID device = 0;
  SDL_AudioSpec obtained;
  std::string error;
};

static SDL_AudioStream* bridgeStream = NULL;  // engine rate -> device rate when they differ
static int bridgeEngineRate = 0;
static int bridgeFrames = 0;

static void SDLCALL audioTrampoline(void*, Uint8* stream, int length) {
  enableFlushToZero();
  AudioCallback* callback = audioCallbackPtr.load(std::memory_order_acquire);
  if (!callback) { memset(stream, 0, length); return; }
  if (!bridgeStream) { callback((int16_t*)stream, length / 4); return; }
  static std::vector<int16_t> scratch;
  if (scratch.size() < (size_t)bridgeFrames * 2) scratch.resize((size_t)bridgeFrames * 2);
  while (SDL_AudioStreamAvailable(bridgeStream) < length) {
    callback(scratch.data(), bridgeFrames);
    SDL_AudioStreamPut(bridgeStream, scratch.data(), bridgeFrames * 4);
  }
  SDL_AudioStreamGet(bridgeStream, stream, length);
}

static void openWorker(std::shared_ptr<OpenAttempt> attempt, int rate, int samples) {
  rgnanoLog("audio worker: SDL_InitSubSystem(AUDIO)");
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    std::lock_guard<std::mutex> lock(attempt->mutex);
    attempt->error = SDL_GetError();
    attempt->finished = true;
    attempt->done.notify_all();
    return;
  }
  rgnanoLog("audio worker: driver=%s AUDIODEV=%s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
            getenv("AUDIODEV") ? getenv("AUDIODEV") : "(unset)");
  SDL_AudioSpec want, have;
  SDL_zero(want);
  want.freq = rate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = (Uint16)samples;
  want.callback = audioTrampoline;
  rgnanoLog("audio worker: SDL_OpenAudioDevice(default via AUDIODEV) %d Hz S16 2ch %d", rate, samples);
  SDL_zero(have);
  SDL_AudioDeviceID device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
  std::lock_guard<std::mutex> lock(attempt->mutex);
  attempt->device = device;
  attempt->obtained = have;
  if (!device) attempt->error = SDL_GetError();
  attempt->finished = true;
  attempt->done.notify_all();
}

static SDL_AudioDeviceID tryOpen(const char* label, int rate, int samples) {
  rgnanoLog("audio: attempt '%s' (timeout %d ms)", label, kOpenTimeoutMs);
  auto attempt = std::make_shared<OpenAttempt>();
  long long started = nowUs();
  std::thread(openWorker, attempt, rate, samples).detach();
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
  return attempt->device;
}

static bool openAudio(int rate, int samples) {
  const char* forced = getenv("RGNANO_AUDIO_DEVICE");  // ALSA PCM name, skips the probe
  const char* drv = getenv("SDL_AUDIODRIVER");
  bool probeSkipped = (drv && drv[0] && strcmp(drv, "alsa") != 0);  // dummy/test drivers
  std::string name;
  int devRate = rate;
  if (forced && forced[0]) {
    name = forced;
    rgnanoLog("audio: RGNANO_AUDIO_DEVICE forces '%s', probe skipped", forced);
  } else if (probeSkipped) {
    rgnanoLog("audio: SDL_AUDIODRIVER=%s, probe skipped", drv);
  } else {
    if (!probeWinner.valid && !runProbe(rate, samples)) {
      rgnanoLog("audio: NO DEVICE. Running silent with a software clock");
      audioFailed = true;
      audioRate = rate;
      audioSamples = samples;
      return false;
    }
    name = probeWinner.name;
    // Native rate worked: let the plug layer resample other rates. Only the
    // fallback rate (48000) needs the engine-to-device bridge.
    devRate = (probeWinner.devRate == probeWinner.reqRate) ? rate : probeWinner.devRate;
  }
  if (!name.empty()) setenv("AUDIODEV", name.c_str(), 1);
  unsetenv("SDL_AUDIO_DEVICE_NAME");
  SDL_AudioDeviceID device = tryOpen(name.empty() ? "default" : name.c_str(), devRate, samples);
  if (device && devRate != rate) {
    bridgeStream = SDL_NewAudioStream(AUDIO_S16SYS, 2, rate, AUDIO_S16SYS, 2, devRate);
    bridgeEngineRate = rate;
    bridgeFrames = samples;
    rgnanoLog("audio: bridging engine %d Hz -> device %d Hz (%s)", rate, devRate, bridgeStream ? "ok" : "STREAM FAILED");
    if (!bridgeStream) { SDL_CloseAudioDevice(device); device = 0; }
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
  audioDeviceName = name;
  rgnanoLog("audio: CHOSEN device '%s', device rate %d Hz, engine rate %d Hz", name.empty() ? "default" : name.c_str(), devRate, rate);
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
  if (bridgeStream) { SDL_FreeAudioStream(bridgeStream); bridgeStream = NULL; }
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
  logSync = false;
}

// ---------------------------------------------------------------------------
// Presenter: area-downscale to 240x240, RGB565, /dev/fb0 or a dump file
// ---------------------------------------------------------------------------

void rgnanoCanvasSize(int* width, int* height) {
  int w = 320, h = 240;  // 40x20 grid of the 8x12 font, squashed 0.75x horizontally to 240x240
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
rgnanoscale::GenericScaler genericScaler;
std::vector<uint32_t> prevSrc;   // last presented source, to find changed rows
int prevW = 0, prevH = 0;
bool prevValid = false;
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
unsigned long frameCount = 0, skippedFrames = 0;
unsigned long long sumRows = 0;
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
  long long t0 = nowUs();
  const size_t words = (size_t)w * h;
  if (prevW != w || prevH != h) {
    prevSrc.assign(words, 0);
    prevW = w; prevH = h;
    prevValid = false;
  }
  const bool fast = (w == 320 && h == 240 && RGNANO_PHYSICAL_W == 240 && RGNANO_PHYSICAL_H == 240);
  // Changed destination rows [rowLo, rowHi). Fast path: vertical is 1:1, so source
  // row y maps to destination row y and only changed rows are converted. Generic
  // path: any change reconverts the whole frame.
  static bool rowChanged[RGNANO_PHYSICAL_H];
  int changedRows = 0;
  if (fast) {
    for (int y = 0; y < 240; y++) {
      const uint32_t* s = argb + (size_t)y * 320;
      uint32_t* p = prevSrc.data() + (size_t)y * 320;
      bool ch = !prevValid || rgnanoscale::rowsDiffer(s, p, 320);
      rowChanged[y] = ch;
      if (ch) {
        memcpy(p, s, 320 * 4);
        rgnanoscale::squashRow320to240(s, frame565 + y * RGNANO_PHYSICAL_W);
        changedRows++;
      }
    }
  } else {
    bool ch = !prevValid || rgnanoscale::rowsDiffer(argb, prevSrc.data(), (int)words);
    if (ch) {
      memcpy(prevSrc.data(), argb, words * 4);
      genericScaler.scale(argb, w, h, RGNANO_PHYSICAL_W, RGNANO_PHYSICAL_H, frame565);
      changedRows = RGNANO_PHYSICAL_H;
    }
    for (int y = 0; y < RGNANO_PHYSICAL_H; y++) rowChanged[y] = ch;
  }
  prevValid = true;
  const bool skipped = changedRows == 0;
  if (!skipped) haveFrame = true;
  long long t1 = nowUs();

  if (skipped) {
    if (haveFrame && (dumpPath || ppmPath)) flushDumps(false);  // keep debug dumps fresh on static screens
  } else if (dumpPath || ppmPath) {
    flushDumps(false);
  } else {
    if (!fbTried) openFramebuffer();
    if (fbMemory) {
      // Copy only runs of changed rows: the panel is SPI-backed.
      for (int y = 0; y < RGNANO_PHYSICAL_H;) {
        if (!rowChanged[y]) { y++; continue; }
        int y1 = y;
        while (y1 < RGNANO_PHYSICAL_H && rowChanged[y1]) y1++;
        if (fbStride == RGNANO_PHYSICAL_W * 2) {
          memcpy(fbMemory + (size_t)y * fbStride, frame565 + y * RGNANO_PHYSICAL_W, (size_t)(y1 - y) * RGNANO_PHYSICAL_W * 2);
        } else {
          for (int r = y; r < y1; r++)
            memcpy(fbMemory + (size_t)r * fbStride, frame565 + r * RGNANO_PHYSICAL_W, RGNANO_PHYSICAL_W * 2);
        }
        y = y1;
      }
    }
  }
  long long t2 = nowUs();
  sumRows += changedRows;
  if (skipped) skippedFrames++;

  unsigned scaleUs = (unsigned)(t1 - t0), blitUs = (unsigned)(t2 - t1);
  sumScaleUs += scaleUs; sumBlitUs += blitUs; sumReadUs += readbackUs;
  if (scaleUs > maxScaleUs) maxScaleUs = scaleUs;
  if (blitUs > maxBlitUs) maxBlitUs = blitUs;
  if (readbackUs > maxReadUs) maxReadUs = readbackUs;
  if (++frameCount % 300 == 0) {
    rgnanoLog("present: %lu frames, avg us read=%llu scale=%llu blit=%llu, max read=%u scale=%u blit=%u, skipped=%lu rows_blitted avg=%llu",
              frameCount, sumReadUs / 300, sumScaleUs / 300, sumBlitUs / 300, maxReadUs, maxScaleUs, maxBlitUs,
              skippedFrames, sumRows / 300);
    sumScaleUs = sumBlitUs = sumReadUs = 0;
    skippedFrames = 0; sumRows = 0;
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
