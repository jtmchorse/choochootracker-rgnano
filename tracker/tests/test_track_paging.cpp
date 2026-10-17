#include "doctest.h"
#include "common.h"
#include "app.h"
#include "app_ui_mock.h"
#include "corelib_file.h"
#include "track_paging.h"
#include "../platforms/shared/rgnano_keymap.h"

#include <chrono>
#include <filesystem>
#include <string>

TEST_CASE("track paging: column to track mapping") {
  // Paged: slot 0..3 on the page the track belongs to, -1 elsewhere.
  for (int t = 0; t < 8; ++t) {
    CHECK(trackSlot(t, 0, 1) == (t < 4 ? t : -1));
    CHECK(trackSlot(t, 1, 1) == (t >= 4 ? t - 4 : -1));
    CHECK(trackSlot(t, 0, 0) == t);  // unpaged view: slot is the track
  }
  CHECK(slotTrack(0, 0, 8) == 0);
  CHECK(slotTrack(3, 0, 8) == 3);
  CHECK(slotTrack(0, 1, 8) == 4);
  CHECK(slotTrack(3, 1, 8) == 7);
  CHECK(slotTrack(3, 1, 6) == -1);  // fewer tracks than a full second page
  CHECK(trackPageFirst(1) == 4);
  CHECK(trackPageLast(1, 8) == 7);
  CHECK(trackPageLast(1, 6) == 5);
  CHECK(trackPageCount(8) == 2);
  CHECK(trackPageCount(4) == 1);
  CHECK(trackViewPaged(TRACK_VIEW_4X2, 8));
  CHECK_FALSE(trackViewPaged(TRACK_VIEW_ALL, 8));
  CHECK_FALSE(trackViewPaged(TRACK_VIEW_4X2, 4));
}

TEST_CASE("track paging: page flips at the edges") {
  // The D-pad moves the absolute track index; the page follows it.
  CHECK(trackPageOf(3) == 0);
  CHECK(trackPageOf(4) == 1);
  CHECK(trackPageOf(7) == 1);
  // Explicit page move keeps the slot and is a no-op at the ends.
  CHECK(trackPageMove(2, +1, 8) == 6);
  CHECK(trackPageMove(6, -1, 8) == 2);
  CHECK(trackPageMove(0, -1, 8) == 0);
  CHECK(trackPageMove(7, +1, 8) == 7);
  CHECK(trackPageMove(3, +1, 6) == 5);  // clamped to the last track
}

TEST_CASE("track paging: Shift+L / Shift+R chord keys") {
  CHECK(rgnanoPageChordDirection(RGNANO_KEY_L1) == -1);
  CHECK(rgnanoPageChordDirection(RGNANO_KEY_R1) == 1);
  for (int key : {RGNANO_KEY_UP, RGNANO_KEY_DOWN, RGNANO_KEY_LEFT, RGNANO_KEY_RIGHT, RGNANO_KEY_A,
                  RGNANO_KEY_B, RGNANO_KEY_X, RGNANO_KEY_Y, RGNANO_KEY_START, RGNANO_KEY_SELECT, RGNANO_KEY_MENU})
    CHECK(rgnanoPageChordDirection(key) == 0);
  // Not one of fkgpiod's stock FN chords (those arrive as j, i, h).
  CHECK(rgnanoFnChordDirection(RGNANO_KEY_L1) == 0);
  CHECK(rgnanoFnChordDirection(RGNANO_KEY_R1) == 0);
}

#ifdef RGNANO_BUILD
namespace {
void padKey(int sym, bool down) {
  MainLoopEventData event = {};
  event.type = down ? MainLoopEvent::keyDown : MainLoopEvent::keyUp;
  event.data.input = (InputCode){InputDeviceType::keyboard, sym};
  appOnEvent(event);
}
void tapKey(int sym) { padKey(sym, true); padKey(sym, false); }
void chord(int sym) { padKey('k', true); padKey(sym, true); padKey(sym, false); padKey('k', false); }

struct PagingFixture {
  AppSettings savedSettings = appSettings;
  ChipNomadState* savedState = chipnomadState;
  std::filesystem::path originalPath = std::filesystem::current_path();
  std::filesystem::path testPath = std::filesystem::temp_directory_path() /
    ("choochootracker-paging-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::string realSettingsPath;
  PagingFixture() {
    std::filesystem::create_directories(testPath);
    std::filesystem::current_path(testPath);
    char dir[PATH_LENGTH];
    if (fileGetDefaultDirectory(dir, sizeof(dir)) == 0) realSettingsPath = std::string(dir) + "/settings.txt";
    if (!realSettingsPath.empty()) std::filesystem::remove(realSettingsPath);
    initDefaultAppSettings();
    inputRawCallback = nullptr;
    KeyMapping fresh = {};
    rgnanoSetSlot0(&fresh);
    appSettings.keyMapping = fresh;
    appSetup();
    currentScreen = &screenSong;
    screenSetup(&screenSong, 0);
  }
  ~PagingFixture() {
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

TEST_CASE_FIXTURE(PagingFixture, "track paging: RG Nano defaults to the 4x2 view") {
  CHECK(appSettings.trackView == TRACK_VIEW_4X2);
  CHECK(trackViewPaged(appSettings.trackView, 8));
}

TEST_CASE_FIXTURE(PagingFixture, "track paging: Shift+L and Shift+R reach the Song screen") {
  mockPageMoves = 0;
  chord('n');
  CHECK(mockPageMoves == 1);
  CHECK(mockPageDir == 1);
  chord('m');
  CHECK(mockPageMoves == 2);
  CHECK(mockPageDir == -1);
}

TEST_CASE_FIXTURE(PagingFixture, "track paging: L and R alone, or off the Song screen, do not page") {
  mockPageMoves = 0;
  tapKey('n');
  tapKey('m');
  CHECK(mockPageMoves == 0);
  currentScreen = &screenProject;
  chord('n');
  chord('m');
  CHECK(mockPageMoves == 0);
}

TEST_CASE_FIXTURE(PagingFixture, "track paging: the setting round-trips through settings.txt") {
  appSettings.trackView = TRACK_VIEW_ALL;
  REQUIRE(settingsSave() == 0);
  appSettings.trackView = TRACK_VIEW_4X2;
  REQUIRE(settingsLoad() == 0);
  CHECK(appSettings.trackView == TRACK_VIEW_ALL);
}
#endif
