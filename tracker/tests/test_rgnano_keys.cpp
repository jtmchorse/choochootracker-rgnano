#include "doctest.h"
#include "common.h"
#include "app.h"
#include "app_ui_mock.h"
#include "corelib_file.h"
#include "../platforms/shared/rgnano_keymap.h"

#include <chrono>
#include <filesystem>
#include <string>

// The 13 RG Nano keys as SDL delivers them (round 3 probe: sym == ASCII).
namespace {
// kNone: the key is unmapped or consumed before any screen sees it.
const int kNone = -1;
struct Pad { char sym; int key; const char* name; };
const Pad kPad[] = {
  {'u', keyUp, "Up"}, {'d', keyDown, "Down"}, {'l', keyLeft, "Left"}, {'r', keyRight, "Right"},
  {'a', keyEdit, "A"}, {'b', keyOpt, "B"}, {'x', kNone, "X"}, {'y', kNone, "Y"},
  {'m', kNone, "L (motion live, consumed by the app)"}, {'n', kNone, "R (free)"}, {'s', keyPlay, "Start"},
  {'k', keyShift, "Select (fkgpiod FN line)"}, {'q', kNone, "Menu (unmapped, system combos)"},
};

void sdlKey(int sym, bool down) {
  MainLoopEventData event = {};
  event.type = down ? MainLoopEvent::keyDown : MainLoopEvent::keyUp;
  event.data.input = (InputCode){InputDeviceType::keyboard, sym};
  appOnEvent(event);
}

struct RgnanoKeyFixture {
  AppSettings savedSettings = appSettings;
  ChipNomadState* savedState = chipnomadState;
  std::filesystem::path originalPath = std::filesystem::current_path();
  std::filesystem::path testPath = std::filesystem::temp_directory_path() /
    ("choochootracker-rgnano-keys-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::string realSettingsPath;

  RgnanoKeyFixture() {
    std::filesystem::create_directories(testPath);
    std::filesystem::current_path(testPath);
    char dir[PATH_LENGTH];
    if (fileGetDefaultDirectory(dir, sizeof(dir)) == 0) realSettingsPath = std::string(dir) + "/settings.txt";
    if (!realSettingsPath.empty()) std::filesystem::remove(realSettingsPath);
    initDefaultAppSettings();
    inputRawCallback = nullptr;
  }
  void boot() {
    appSetup();
    currentScreen = &screenSong;
  }
  ~RgnanoKeyFixture() {
    inputRawCallback = nullptr;
    if (chipnomadState && chipnomadState != savedState) appCleanup();
    chipnomadState = savedState;
    appSettings = savedSettings;
    std::filesystem::current_path(originalPath);
    std::filesystem::remove_all(testPath);
    if (!realSettingsPath.empty()) std::filesystem::remove(realSettingsPath);
  }
};
}

TEST_CASE_FIXTURE(RgnanoKeyFixture, "RG Nano: all 13 keys translate to the intended abstract keys") {
  KeyMapping fresh = {};
  rgnanoSetSlot0(&fresh);
  appSettings.keyMapping = fresh;
  boot();
  for (const Pad& pad : kPad) {
    mockLastInputKeys = kNone;
    sdlKey(pad.sym, true);
    INFO("key " << pad.name << " '" << pad.sym << "'");
    CHECK(mockLastInputKeys == pad.key);
    sdlKey(pad.sym, false);
  }
}

TEST_CASE_FIXTURE(RgnanoKeyFixture, "RG Nano: Select+Left and Select+Right navigate screens with real keysyms") {
  KeyMapping fresh = {};
  rgnanoSetSlot0(&fresh);
  appSettings.keyMapping = fresh;
  boot();
  for (char dir : {'l', 'r'}) {
    screenSetup(&screenProject, 0);
    sdlKey('k', true);
    sdlKey(dir, true);
    CHECK(currentScreen == (dir == 'l' ? &screenMixer : &screenChain));
    sdlKey(dir, false);
    sdlKey('k', false);
  }
}

namespace {
// What the Key Mapping screen does when a capture completes (screen_keymapping.cpp
// applyPendingCapture): write the captured key into the slot; a conflicting slot
// takes the previous value.
void bindLikeKeyMappingScreen(InputCode* slot, int sym) {
  KeyMapping* m = &appSettings.keyMapping;
  InputCode* all[] = {m->keyUp, m->keyDown, m->keyLeft, m->keyRight, m->keyEdit, m->keyOpt,
                      m->keyPlay, m->keyShift, m->keyMotionLive, m->keyMotionRecord, m->keyMotionErase};
  InputCode previous = *slot;
  for (InputCode* row : all)
    for (int col = 0; col < 3; col++)
      if (&row[col] != slot && row[col].deviceType == InputDeviceType::keyboard && row[col].code == sym) row[col] = previous;
  *slot = (InputCode){InputDeviceType::keyboard, sym};
}

bool sameCode(InputCode a, InputCode b) { return a.deviceType == b.deviceType && a.code == b.code; }
}

TEST_CASE_FIXTURE(RgnanoKeyFixture, "RG Nano: a fresh install gets Select on k") {
  settingsLoad();                       // no settings.txt
  CHECK(rgnanoMigrateKeymap(&appSettings) == 0);  // missing keymap: left for appSetup
  CHECK(appSettings.keymapVersion == RGNANO_SETTINGS_VERSION);
  rgnanoSetSlot0(&appSettings.keyMapping);        // what inputInitDefaultKeyMapping does
  CHECK(sameCode(appSettings.keyMapping.keyShift[0], {InputDeviceType::keyboard, 'k'}));
}

TEST_CASE_FIXTURE(RgnanoKeyFixture, "RG Nano: a 4.x settings.txt with the old Select on q is moved to k") {
  KeyMapping v4 = {};
  rgnanoSetSlot0(&v4);
  v4.keyShift[0] = {InputDeviceType::keyboard, 'q'};  // the old default
  appSettings.keyMapping = v4;
  REQUIRE(settingsSave() == 0);         // keymapVersion 0, as a build without the field wrote it
  appSettings.keyMapping = {};
  REQUIRE(settingsLoad() == 0);
  appSettings.keymapVersion = 0;
  CHECK(rgnanoMigrateKeymap(&appSettings) == 1);
  CHECK(sameCode(appSettings.keyMapping.keyShift[0], {InputDeviceType::keyboard, 'k'}));
  CHECK(rgnanoMigrateKeymap(&appSettings) == 0);
  boot();
  mockLastInputKeys = kNone;
  sdlKey('k', true);
  CHECK(mockLastInputKeys == keyShift);
  sdlKey('k', false);
  CHECK_FALSE(appInputWantsKey({InputDeviceType::keyboard, 'q'}));  // MENU is not mapped
}

TEST_CASE_FIXTURE(RgnanoKeyFixture, "RG Nano: v2 -> v3 migration only moves the old default Select") {
  const InputDeviceType kb = InputDeviceType::keyboard;
  KeyMapping v2 = {};
  rgnanoSetSlot0(&v2);
  v2.keyShift[0] = {kb, 'q'};
  appSettings.keyMapping = v2;
  appSettings.keymapVersion = 2;
  CHECK(rgnanoMigrateKeymap(&appSettings) == 1);
  CHECK(appSettings.keymapVersion == 3);
  CHECK(sameCode(appSettings.keyMapping.keyShift[0], {kb, 'k'}));
  CHECK(sameCode(appSettings.keyMapping.keyUp[0], {kb, 'u'}));
  // A v2 map where Shift was deliberately moved elsewhere is left alone.
  appSettings.keyMapping = v2;
  appSettings.keyMapping.keyShift[0] = {kb, 'y'};
  appSettings.keymapVersion = 2;
  CHECK(rgnanoMigrateKeymap(&appSettings) == 0);
  CHECK(sameCode(appSettings.keyMapping.keyShift[0], {kb, 'y'}));
  CHECK(appSettings.keymapVersion == 3);
}

TEST_CASE("RG Nano: stock Fn chords map to Shift+direction in the fallback") {
  CHECK(rgnanoFnChordDirection('j') == 'l');
  CHECK(rgnanoFnChordDirection('i') == 'r');
  CHECK(rgnanoFnChordDirection('h') == 'd');
  CHECK(rgnanoFnChordDirection('u') == 0);
  CHECK(rgnanoFnChordDirection('k') == 0);
}

TEST_CASE_FIXTURE(RgnanoKeyFixture, "RG Nano: a settings.txt with a foreign (desktop) keymap is rebuilt once") {
  KeyMapping desktop = {};
  const InputDeviceType kb = InputDeviceType::keyboard;
  desktop.keyUp[0] = {kb, 1073741906}; desktop.keyDown[0] = {kb, 1073741905};
  desktop.keyLeft[0] = {kb, 1073741904}; desktop.keyRight[0] = {kb, 1073741903};
  desktop.keyEdit[0] = {kb, 'x'}; desktop.keyOpt[0] = {kb, 'z'}; desktop.keyPlay[0] = {kb, ' '};
  desktop.keyShift[0] = {kb, 1073742049};
  desktop.keyMotionLive[0] = {kb, 'q'}; desktop.keyMotionRecord[0] = {kb, 'w'}; desktop.keyMotionErase[0] = {kb, 'e'};
  appSettings.keyMapping = desktop;
  REQUIRE(settingsSave() == 0);
  appSettings.keyMapping = {};
  REQUIRE(settingsLoad() == 0);
  appSettings.keymapVersion = 0;
  CHECK(rgnanoMigrateKeymap(&appSettings) == 1);
  CHECK(sameCode(appSettings.keyMapping.keyShift[0], {kb, 'k'}));
  CHECK(sameCode(appSettings.keyMapping.keyUp[0], {kb, 'u'}));
  CHECK(appSettings.keymapVersion == RGNANO_SETTINGS_VERSION);
  CHECK(rgnanoMigrateKeymap(&appSettings) == 0);  // one time only
}

TEST_CASE_FIXTURE(RgnanoKeyFixture, "RG Nano: a deliberate remap survives the migration and restarts") {
  const InputDeviceType kb = InputDeviceType::keyboard;
  KeyMapping custom = {};
  rgnanoSetSlot0(&custom);
  custom.keyShift[0] = {kb, 'n'};       // the shift-r1 preset
  custom.keyMotionLive[0] = {kb, 'q'};  // MENU bound to stick-live
  custom.keyEdit[1] = {kb, 'y'};        // extra slots are kept too
  appSettings.keyMapping = custom;
  appSettings.keymapVersion = 0;        // saved by an older build
  REQUIRE(settingsSave() == 0);
  appSettings.keyMapping = {};
  appSettings.keymapVersion = 99;
  REQUIRE(settingsLoad() == 0);
  CHECK(rgnanoMigrateKeymap(&appSettings) == 0);
  CHECK(sameCode(appSettings.keyMapping.keyShift[0], {kb, 'n'}));
  CHECK(sameCode(appSettings.keyMapping.keyMotionLive[0], {kb, 'q'}));
  CHECK(sameCode(appSettings.keyMapping.keyEdit[1], {kb, 'y'}));
  REQUIRE(settingsSave() == 0);         // second restart
  appSettings.keyMapping = {};
  REQUIRE(settingsLoad() == 0);
  CHECK(rgnanoMigrateKeymap(&appSettings) == 0);
  CHECK(sameCode(appSettings.keyMapping.keyShift[0], {kb, 'n'}));
  boot();
  mockLastInputKeys = kNone;
  sdlKey('n', true);
  CHECK(mockLastInputKeys == keyShift);
  sdlKey('n', false);
}

TEST_CASE_FIXTURE(RgnanoKeyFixture, "RG Nano: every one of the 13 keys can be bound by the remap screen and survives a restart") {
  const char syms[] = {'u', 'd', 'l', 'r', 'a', 'b', 'x', 'y', 'm', 'n', 's', 'q', 'k'};
  for (char sym : syms) {
    INFO("binding '" << sym << "' to Shift");
    KeyMapping fresh = {};
    rgnanoSetSlot0(&fresh);
    appSettings.keyMapping = fresh;
    appSettings.keymapVersion = RGNANO_SETTINGS_VERSION;
    bindLikeKeyMappingScreen(&appSettings.keyMapping.keyShift[0], sym);
    REQUIRE(settingsSave() == 0);
    appSettings.keyMapping = {};
    REQUIRE(settingsLoad() == 0);
    CHECK(rgnanoMigrateKeymap(&appSettings) == 0);
    CHECK(sameCode(appSettings.keyMapping.keyShift[0], {InputDeviceType::keyboard, sym}));
    if (chipnomadState && chipnomadState != savedState) appCleanup();
    boot();
    CHECK(appInputWantsKey({InputDeviceType::keyboard, sym}));
    mockLastInputKeys = kNone;
    sdlKey(sym, true);
    CHECK(mockLastInputKeys == keyShift);
    sdlKey(sym, false);
  }
  // Select (K) is mapped by default; MENU (Q) is not, until the user binds it.
  KeyMapping fresh = {};
  rgnanoSetSlot0(&fresh);
  appSettings.keyMapping = fresh;
  CHECK(appInputWantsKey({InputDeviceType::keyboard, 'k'}));
  CHECK_FALSE(appInputWantsKey({InputDeviceType::keyboard, 'q'}));
}
