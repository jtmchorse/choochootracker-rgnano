#ifndef RGNANO_KEYMAP_H
#define RGNANO_KEYMAP_H

// RG Nano (FunKey-OS) button layout, kept free of SDL so the native tests can
// include it. The buttons are a uinput keyboard ("ample") fed by fkgpiod;
// SDL's evdev backend turns KEY_<letter> into the lower-case ASCII keysym, and
// SDLK_<letter> is that same ASCII value, so plain char literals are exact.
// Select is the MENU button (fkgpiod.conf: MAP MENU TO KEY KEY_Q).
#define RGNANO_KEY_UP      'u'
#define RGNANO_KEY_DOWN    'd'
#define RGNANO_KEY_LEFT    'l'
#define RGNANO_KEY_RIGHT   'r'
#define RGNANO_KEY_A       'a'
#define RGNANO_KEY_B       'b'
#define RGNANO_KEY_X       'x'
#define RGNANO_KEY_Y       'y'
#define RGNANO_KEY_L1      'm'
#define RGNANO_KEY_R1      'n'
#define RGNANO_KEY_L2      'v'   // Fn+L
#define RGNANO_KEY_R2      'o'   // Fn+R
#define RGNANO_KEY_SELECT  'q'
#define RGNANO_KEY_START   's'
#define RGNANO_KEY_MENU    'k'   // Fn: swallowed, never mapped

// Bump when the saved RG Nano key mapping must be rebuilt from the defaults.
#define RGNANO_SETTINGS_VERSION 2

#ifdef __cplusplus
#include "common.h"

static inline void rgnanoSetSlot0(KeyMapping* m) {
  const InputDeviceType kb = InputDeviceType::keyboard;
  m->keyUp[0] = (InputCode){kb, RGNANO_KEY_UP};
  m->keyDown[0] = (InputCode){kb, RGNANO_KEY_DOWN};
  m->keyLeft[0] = (InputCode){kb, RGNANO_KEY_LEFT};
  m->keyRight[0] = (InputCode){kb, RGNANO_KEY_RIGHT};
  m->keyOpt[0] = (InputCode){kb, RGNANO_KEY_B};
  m->keyPlay[0] = (InputCode){kb, RGNANO_KEY_START};
  m->keyShift[0] = (InputCode){kb, RGNANO_KEY_SELECT};
  m->keyMotionLive[0] = (InputCode){kb, RGNANO_KEY_L1};
  m->keyMotionRecord[0] = (InputCode){kb, RGNANO_KEY_L2};
  m->keyMotionErase[0] = (InputCode){kb, RGNANO_KEY_R2};
  m->keyEdit[0] = (InputCode){kb, RGNANO_KEY_A};
}

// True for the 13 keys the fkgpiod keyboard can produce (u d l r a b x y m n s q k).
static inline int rgnanoIsHardwareKey(int32_t code) {
  switch (code) {
    case RGNANO_KEY_UP: case RGNANO_KEY_DOWN: case RGNANO_KEY_LEFT: case RGNANO_KEY_RIGHT:
    case RGNANO_KEY_A: case RGNANO_KEY_B: case RGNANO_KEY_X: case RGNANO_KEY_Y:
    case RGNANO_KEY_L1: case RGNANO_KEY_R1: case RGNANO_KEY_START: case RGNANO_KEY_SELECT:
    case RGNANO_KEY_MENU:
    case RGNANO_KEY_L2: case RGNANO_KEY_R2:  // Fn+L / Fn+R chords
      return 1;
  }
  return 0;
}

// One-time keymap migration, keyed on appSettings.keymapVersion (settings.txt
// "keymapVersion"). It must never overwrite a deliberate remap, so it only
// acts when keyboard slot 0 of a button holds something this hardware cannot
// produce (an upstream/desktop default such as the arrow keys or LShift, or an
// empty slot while the rest of the map is set). Such a code can only come from
// a stale or foreign settings.txt: the remap screen captures events from the
// 'ample' keyboard only, so every deliberate remap is one of the 13 keys.
// A missing keymap (keyUp[0] unset) is left alone: appSetup builds the
// defaults (and applies keymap-preset.txt). Returns 1 if slot 0 was rebuilt.
static inline int rgnanoMigrateKeymap(AppSettings* settings) {
  if (settings->keymapVersion >= RGNANO_SETTINGS_VERSION) return 0;
  settings->keymapVersion = RGNANO_SETTINGS_VERSION;
  KeyMapping* m = &settings->keyMapping;
  if (m->keyUp[0].deviceType == InputDeviceType::none) return 0;
  const InputCode* slots[] = {
    &m->keyUp[0], &m->keyDown[0], &m->keyLeft[0], &m->keyRight[0], &m->keyEdit[0], &m->keyOpt[0],
    &m->keyPlay[0], &m->keyShift[0], &m->keyMotionLive[0], &m->keyMotionRecord[0], &m->keyMotionErase[0],
  };
  int stale = 0;
  for (const InputCode* slot : slots)
    if (slot->deviceType != InputDeviceType::keyboard || !rgnanoIsHardwareKey(slot->code)) stale = 1;
  if (!stale) return 0;
  rgnanoSetSlot0(m);
  return 1;
}
#endif

#endif
