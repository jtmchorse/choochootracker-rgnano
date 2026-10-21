// ALSA capture-device discovery and hot-plug bookkeeping for the RG Nano USB
// audio-in path. No SDL and no alsa-lib here, so the host tests can drive it
// against a fixture /proc/asound tree.
//
// Why this exists: SDL caches its capture device list until the audio
// subsystem is re-initialised, and the upstream refresh does exactly that with
// SDL_QuitSubSystem(SDL_INIT_AUDIO). On the Nano the same subsystem owns the
// playback device (early-open attach, USB DAC), so re-initialising it kills
// playback or hangs in the kernel USB path. We re-scan /proc/asound instead and
// never touch the SDL audio subsystem.
#pragma once

#include <stddef.h>

#define ALSA_CAPTURE_MAX_CARDS 16
#define ALSA_CAPTURE_NAME_LENGTH 64

struct AlsaCaptureCard {
  int card;                              // ALSA card index (changes with plug order)
  int pcm;                               // first capture PCM number on that card
  char name[ALSA_CAPTURE_NAME_LENGTH];   // stable long name, e.g. "Sonos USB-C Line-in Adapter"
};

// Parses one "/proc/asound/cards" text. Fills id/name per card (no PCM
// check). Returns the number of cards parsed.
int alsaParseCards(const char* text, AlsaCaptureCard* out, int maxCards);

// Reads <root>/cards and keeps the cards that have a <root>/card<N>/pcm<M>c
// directory. Returns the count (0 when root is missing).
int alsaScanCaptureCards(const char* root, AlsaCaptureCard* out, int maxCards);

// "card N: name": what the Audio Input menu shows.
void alsaCaptureLabel(const AlsaCaptureCard& c, char* buffer, int bufferSize);
// The identity that is saved in settings.txt: a label without its "card N: "
// prefix (a label without the prefix is returned unchanged).
const char* alsaCaptureKey(const char* labelOrKey);
// Index in cards[0..count) whose name matches the label/key, or -1.
int alsaCaptureFindByName(const AlsaCaptureCard* cards, int count, const char* labelOrKey);
// "plughw:N,M" for the SDL alsa driver (AUDIODEV), same as playback.
void alsaCaptureDeviceName(const AlsaCaptureCard& c, char* buffer, int bufferSize);

// Hot-plug / refresh policy, with the side effects injected so tests can
// count them.
struct AlsaCaptureOps {
  int (*scan)(AlsaCaptureCard* out, int maxCards);  // default: scan /proc/asound
  void (*closeCapture)(void);                       // closes ONLY the capture device
  void (*quitAudioSubsystem)(void);                 // SDL_QuitSubSystem(SDL_INIT_AUDIO); never used on RG Nano
};

// Current list from the last scan (indices are what the menu shows).
int alsaCaptureCount(void);
const AlsaCaptureCard* alsaCaptureCard(int index);
// Re-scans the list. Does not touch the armed device.
int alsaCaptureRescan(const AlsaCaptureOps& ops);
// Name (key) of the capture device currently open; "" when none.
void alsaCaptureSetOpenName(const char* labelOrKey);
// Re-scans and, when the open capture device is gone, closes only the capture
// device and raises the lost flag. Returns 0. Never calls quitAudioSubsystem
// unless allowSubsystemRestart is set (non-RG-Nano platforms).
int alsaCaptureRefresh(const AlsaCaptureOps& ops, bool allowSubsystemRestart);
// True once after a refresh found the open device missing.
bool alsaCaptureTakeLost(void);
