// RG Nano: every screen, picker and overlay is 30 columns wide and presented 1:1.
// The production screens that link into the test binary are drawn against the recording
// gfx mock; tests/screen_fit.cpp (make -f Makefile.test screen-fit RGNANO=1) does the
// same for every screen, including those the mocked app UI replaces here.
#include "doctest.h"
#include "common.h"
#include "app.h"
#include "app_ui_mock.h"
#include "screens.h"
#include "screen_layout.h"
#include "screen_audio_input.h"
#include "screen_instrument.h"
#include "screen_midi.h"
#include "screen_midi_cc.h"
#include "screen_midi_channel_map.h"
#include "selection_popup.h"
#include "track_paging.h"
#include "corelib_gfx.h"
#include "project_instruments.h"
#include "project_utils.h"
#include <filesystem>
#include <chrono>
#include <string>

extern int mockMaxRight;
void charEditFullDraw(char startChar);

#ifdef RGNANO_BUILD
namespace {
struct AllScreensFixture {
  AppSettings savedSettings = appSettings;
  ChipNomadState* savedState = chipnomadState;
  const AppScreen* savedScreen = currentScreen;
  std::filesystem::path originalPath = std::filesystem::current_path();
  std::filesystem::path testPath = std::filesystem::temp_directory_path() /
    ("choochootracker-allscreens-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  AllScreensFixture() {
    std::filesystem::create_directories(testPath);
    std::filesystem::current_path(testPath);
    initDefaultAppSettings();
    appSetup();
  }
  ~AllScreensFixture() {
    if (chipnomadState && chipnomadState != savedState) appCleanup();
    chipnomadState = savedState;
    currentScreen = savedScreen;
    appSettings = savedSettings;
    std::filesystem::current_path(originalPath);
    std::filesystem::remove_all(testPath);
  }
};

void setType(InstrumentType type) {
  Instrument* in = &chipnomadState->project.instruments[0];
  InstrumentFunctions old = getInstrumentFunctions(in->type);
  if (old.free) old.free(in);
  in->type = type;
  InstrumentFunctions now = getInstrumentFunctions(type);
  if (now.init) now.init(in);
  cInstrument = 0;
}

int drawnRight(const AppScreen* screen) {
  currentScreen = screen;
  mockMaxRight = -1;
  if (screen->setup) screen->setup(0);
  screen->fullRedraw();
  screen->draw();
  return mockMaxRight;
}

const AppScreen* const linkedScreens[] = {
  &screenSong, &screenChain, &screenPhrase, &screenGroove, &screenInstrument, &screenFMEdit, &screenMonkEdit,
  &screenSIDEdit, &screenSoundFontEdit, &screenInstrumentPool, &screenModulation, &screenInsertFX, &screenTable,
  &screenAYWavetable, &screenProject, &screenSettings, &screenControlsSettings, &screenAudioSettings,
  &screenTrackVisuals, &screenSynthSettings, &screenMixerSettings, &screenGraphicsSettings, &screenMixer,
  &screenSelectionPopup, &screenColorTheme, &screenKeyMapping, &screenMidi, &screenMidiChannelMap, &screenMidiCC,
  &screenAudioInput, &screenTitle, &screenFileBrowser,
};
}

TEST_CASE("RG Nano: every screen reports 30 columns and the squashed canvas is unreachable") {
  for (const AppScreen* screen : linkedScreens) {
    CHECK(screen->gridCols == 30);
    // A wide overlay (FX picker, character picker) no longer forces the squashed canvas.
    CHECK(screenPresentCols(screen, 0) == 30);
    CHECK(screenPresentCols(screen, 1) == 30);
  }
  CHECK(screenPresentCols(nullptr, 1) == 30);
  for (int cols : {0, 40, 20, -1}) {
    gfxSetPresentUnsquashedCols(cols);
    CHECK(gfxGetPresentUnsquashedCols() == 30);
  }
  CHECK(screenVisibleCols() == 30);
  CHECK(APP_SCREEN_DEFAULT_COLS == 30);
}

TEST_CASE_FIXTURE(AllScreensFixture, "RG Nano: linked screens draw inside 30 columns") {
  const AppScreen* const real[] = {
    &screenControlsSettings, &screenAudioSettings, &screenSynthSettings, &screenMixerSettings,
    &screenGraphicsSettings, &screenSettings, &screenTrackVisuals, &screenMidi, &screenMidiChannelMap,
    &screenMidiCC, &screenAudioInput, &screenInsertFX,
  };
  for (const AppScreen* screen : real) CHECK(drawnRight(screen) <= 29);
}

TEST_CASE_FIXTURE(AllScreensFixture, "RG Nano: FM, SID, SoundFont, Monk editors and the FX picker fit 30 columns for every instrument type") {
  for (int t = 1; t < int(InstrumentType::totalCount); ++t) {
    InstrumentType type = InstrumentType(t);
    if (!getInstrumentDefinition(type)) continue;
    CAPTURE(t);
    setType(type);
    if (fmEditSupported(type)) CHECK(drawnRight(&screenFMEdit) <= 29);
    if (type == InstrumentType::SID) CHECK(drawnRight(&screenSIDEdit) <= 29);
    if (type == InstrumentType::SoundFont) CHECK(drawnRight(&screenSoundFontEdit) <= 29);
    if (type == InstrumentType::Monk) CHECK(drawnRight(&screenMonkEdit) <= 29);
    // The FX picker: its grid and wrapped help must stay inside 30 columns.
    currentScreen = &screenPhrase;
    mockMaxRight = -1;
    fxEditFullDraw((FXId)0, 0, 0);
    CHECK(mockMaxRight <= 29);
    CHECK(fxEditIsOpen());
  }
}

TEST_CASE_FIXTURE(AllScreensFixture, "RG Nano: character picker and selection popup fit 30 columns") {
  currentScreen = &screenSong;
  mockMaxRight = -1;
  charEditFullDraw('A');
  CHECK(mockMaxRight <= 29);
  CHECK(mockMaxRight >= 0);
  static const SelectionItem kids[] = {{"A child with a very long label indeed", 1, nullptr, 0, "helper text long enough to overrun the row", false},
                                       {"Child two", 2, nullptr, 0, nullptr, false}};
  static const SelectionItem cats[] = {{"A category with a long label", 10, kids, 2, nullptr, false},
                                       {"Second", 11, kids, 2, nullptr, true}};
  for (int mode = 0; mode < 4; ++mode) {
    CAPTURE(mode);
    const bool wide = mode == 1, preview = mode == 2, collections = mode == 3;
    selectionPopupSetup("A POPUP TITLE", cats, 2, 1, nullptr, nullptr, wide,
                        preview ? +[](int, bool) {} : nullptr, collections, collections, nullptr, false);
    CHECK(drawnRight(&screenSelectionPopup) <= 29);
  }
}

TEST_CASE_FIXTURE(AllScreensFixture, "RG Nano: appDraw draws no per-track note column or fixed screen map") {
  currentScreen = &screenSong;
  mockMaxRight = -1;
  appDraw();
  CHECK(mockMaxRight <= 29);
}
#endif
