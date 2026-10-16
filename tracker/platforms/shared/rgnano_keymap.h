#ifndef RGNANO_KEYMAP_H
#define RGNANO_KEYMAP_H

// RG Nano (FunKey-OS) button layout, kept free of SDL so the native tests can
// include it. The buttons are a uinput keyboard ("ample") fed by fkgpiod;
// SDL's evdev backend turns KEY_<letter> into the lower-case ASCII keysym, and
// SDLK_<letter> is that same ASCII value, so plain char literals are exact.
// Select is the button fkgpiod calls FN (KEY_K); the MENU button (KEY_Q) is
// left unmapped because the launcher's key map uses it for system combos.
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
#define RGNANO_KEY_SELECT  'k'
#define RGNANO_KEY_START   's'
#define RGNANO_KEY_MENU    'q'   // MENU: unmapped by default (system combos)

// Bump when the saved RG Nano key mapping must be rebuilt from the defaults.
#define RGNANO_SETTINGS_VERSION 4
// v4: motion record/erase removed (no sticks, no L2/R2) and the audio buffer
// default raised to 1024 frames (32 ms at 32 kHz).
#define RGNANO_AUDIO_BUFFER_DEFAULT 1024

// Fallback when the launcher's key map could not be loaded: fkgpiod's stock
// FN combos arrive as j (Fn+Left), i (Fn+Right), h (Fn+Down). Returns the
// direction key they stand for (Shift+direction), or 0. Fn+Up is a daemon
// command (snapshot) and never reaches the app.
static inline int rgnanoFnChordDirection(int sym) {
  switch (sym) {
    case 'j': return RGNANO_KEY_LEFT;
    case 'i': return RGNANO_KEY_RIGHT;
    case 'h': return RGNANO_KEY_DOWN;
  }
  return 0;
}

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
  // Motion record/erase do not exist on this hardware: unbound in every slot.
  for (int i = 0; i < 3; i++) {
    m->keyMotionRecord[i] = (InputCode){InputDeviceType::none, 0};
    m->keyMotionErase[i] = (InputCode){InputDeviceType::none, 0};
  }
  m->keyEdit[0] = (InputCode){kb, RGNANO_KEY_A};
}

// True for the keys the fkgpiod keyboard can produce (u d l r a b x y m n s q k, plus the stock FN chords).
static inline int rgnanoIsHardwareKey(int32_t code) {
  switch (code) {
    case RGNANO_KEY_UP: case RGNANO_KEY_DOWN: case RGNANO_KEY_LEFT: case RGNANO_KEY_RIGHT:
    case RGNANO_KEY_A: case RGNANO_KEY_B: case RGNANO_KEY_X: case RGNANO_KEY_Y:
    case RGNANO_KEY_L1: case RGNANO_KEY_R1: case RGNANO_KEY_START: case RGNANO_KEY_SELECT:
    case RGNANO_KEY_MENU:
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
// defaults (and applies keymap-preset.txt). Returns 1 if a key binding changed (slot 0 rebuilt, Select moved or motion keys cleared).
static inline int rgnanoMigrateKeymap(AppSettings* settings) {
  const int from = settings->keymapVersion;
  if (from >= RGNANO_SETTINGS_VERSION) return 0;
  settings->keymapVersion = RGNANO_SETTINGS_VERSION;
  KeyMapping* m = &settings->keyMapping;
  if (m->keyUp[0].deviceType == InputDeviceType::none) return 0;
  const InputCode* slots[] = {
    &m->keyUp[0], &m->keyDown[0], &m->keyLeft[0], &m->keyRight[0], &m->keyEdit[0], &m->keyOpt[0],
    &m->keyPlay[0], &m->keyShift[0], &m->keyMotionLive[0],
  };
  int stale = 0;
  for (const InputCode* slot : slots)
    if (slot->deviceType != InputDeviceType::keyboard || !rgnanoIsHardwareKey(slot->code)) stale = 1;
  int changed = 0;
  // v4: motion record/erase are gone; clear whatever an older map bound.
  for (int i = 0; i < 3; i++) {
    if (m->keyMotionRecord[i].deviceType != InputDeviceType::none || m->keyMotionErase[i].deviceType != InputDeviceType::none) changed = 1;
    m->keyMotionRecord[i] = (InputCode){InputDeviceType::none, 0};
    m->keyMotionErase[i] = (InputCode){InputDeviceType::none, 0};
  }
  // v4: the old 512-frame default (16 ms) crackled on the device; one-time move
  // to 1024. Only values below the new default are raised, and only once.
  if (from < 4 && settings->audioBufferSize < RGNANO_AUDIO_BUFFER_DEFAULT) {
    settings->audioBufferSize = RGNANO_AUDIO_BUFFER_DEFAULT;  // not counted in the return value
  }
  if (stale) {
    rgnanoSetSlot0(m);
    return 1;
  }
  // v3: Select moved from Q (the old default) to K. Only the old default is
  // moved; any other Shift binding is a deliberate remap and stays.
  if (from < 3 && m->keyShift[0].deviceType == InputDeviceType::keyboard && m->keyShift[0].code == 'q') {
    m->keyShift[0] = (InputCode){InputDeviceType::keyboard, RGNANO_KEY_SELECT};
    return 1;
  }
  return changed;
}
#endif

#endif
