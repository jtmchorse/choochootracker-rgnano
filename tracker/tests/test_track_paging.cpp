#include "doctest.h"
#include "common.h"
#include "app.h"
#include "app_ui_mock.h"
#include "corelib_file.h"
#include "track_paging.h"
#include "screens.h"
#include "screen_layout.h"
#include <algorithm>
#include "corelib_gfx.h"
#include "../platforms/shared/rgnano_keymap.h"

#include <chrono>
#include <fstream>
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

TEST_CASE("song columns: pixel offset mapping is shared by cursor and draw") {
  // Unpaged: 4 px extra per gap, cell origin 3 + slot*3 at 8 px per cell.
  for (int slot = 0; slot < 8; ++slot) {
    CHECK(songColOffsetPx(slot, 0) == slot * 4);
    CHECK(songColPixelX(slot, 0, 8) == (3 + slot * 3) * 8 + slot * 4);
  }
  CHECK(songColPixelX(7, 0, 8) + 2 * 8 <= 240);   // last column fits the 240 px panel
  CHECK(songColPixelX(0, 0, 8) == 24);            // first column unchanged
  // Adjacent columns are 3 cells + 4 px apart: a 4 px gap more than before.
  CHECK(songColPixelX(1, 0, 8) - songColPixelX(0, 0, 8) == 3 * 8 + 4);
  // Paged view keeps its pitch of 8 cells, no offset.
  for (int slot = 0; slot < 4; ++slot) {
    CHECK(songColOffsetPx(slot, 1) == 0);
    CHECK(songColPixelX(slot, 1, 8) == (3 + slot * 8) * 8);
  }
  // A selection from column a to b is (b - a) gaps wider than the cell span.
  CHECK(songColOffsetPx(5, 0) - songColOffsetPx(2, 0) == 3 * SONG_COL_GAP_PX);
}

TEST_CASE("song columns: gfx pixel offset and rect width are applied and reset") {
  gfxSetPixelOffsetX(12);
  gfxSetRectExtraWidth(8);
  { ScreenOverlayCoordinates overlay; CHECK(gfxGetPixelOffsetX() == 0); CHECK(gfxGetRectExtraWidth() == 0); }
  CHECK(gfxGetPixelOffsetX() == 12);
  CHECK(gfxGetRectExtraWidth() == 8);
  gfxSetPixelOffsetX(0);
  gfxSetRectExtraWidth(0);
}

TEST_CASE("present mode: a grid screen is unsquashed unless a wide overlay is up") {
  AppScreen grid = {};
  grid.gridCols = GRID_SCREEN_COLS;
  AppScreen full = {};
  CHECK(screenPresentCols(&grid, 0) == 30);
  CHECK(screenPresentCols(&grid, 1) == 0);   // FX picker / character picker: squashed canvas
  CHECK(screenPresentCols(&full, 0) == 0);   // Table, Instrument, Settings, popups, help
  CHECK(screenPresentCols(nullptr, 0) == 0);
  CHECK(GRID_SCREEN_COLS * 8 == 240);        // 30 cells of 8 px fill the panel 1:1
  gfxSetPresentUnsquashedCols(30);
  CHECK(gfxGetPresentUnsquashedCols() == 30);
  CHECK(screenVisibleCols() == 30);
  gfxSetPresentUnsquashedCols(0);
  CHECK(screenVisibleCols() == 40);
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

TEST_CASE_FIXTURE(PagingFixture, "track paging: RG Nano defaults to the 8 column view") {
  CHECK(appSettings.trackView == TRACK_VIEW_ALL);
  CHECK_FALSE(trackViewPaged(appSettings.trackView, 8));
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

TEST_CASE("track view migration: 4.8 trackView 1 without a marker becomes 0 exactly once") {
  int view = TRACK_VIEW_4X2, version = 0;
  CHECK(trackViewMigrate(&view, &version, 0) == 1);
  CHECK(view == TRACK_VIEW_ALL);
  CHECK(version == TRACK_VIEW_SETTINGS_VERSION);
  // After the marker exists a deliberate 4x2 is kept.
  view = TRACK_VIEW_4X2;
  CHECK(trackViewMigrate(&view, &version, 1) == 0);
  CHECK(view == TRACK_VIEW_4X2);
  // A marker-less file that already says 8 columns is untouched.
  view = TRACK_VIEW_ALL; version = 0;
  CHECK(trackViewMigrate(&view, &version, 0) == 0);
  CHECK(view == TRACK_VIEW_ALL);
}

TEST_CASE_FIXTURE(PagingFixture, "track view migration through settings.txt: once, then a deliberate 4x2 stays") {
  CHECK(appSettings.trackView == TRACK_VIEW_ALL);  // RG Nano default is 8 columns again
  appSettings.trackView = TRACK_VIEW_4X2;
  REQUIRE(settingsSave() == 0);
  // Strip the marker to look like a round 4.8 file.
  char dir[PATH_LENGTH];
  REQUIRE(fileGetDefaultDirectory(dir, sizeof(dir)) == 0);
  std::string path = std::string(dir) + "/settings.txt";
  std::string text, line;
  { std::ifstream in(path); while (std::getline(in, line)) if (line.rfind("trackViewVersion:", 0) != 0) text += line + "\n"; }
  { std::ofstream out(path, std::ios::trunc); out << text; }
  appSettings.trackView = TRACK_VIEW_4X2;
  REQUIRE(settingsLoad() == 0);
  CHECK(appSettings.trackView == TRACK_VIEW_ALL);   // migrated
  CHECK(appSettings.trackViewVersion == TRACK_VIEW_SETTINGS_VERSION);
  // Deliberate 4x2 after the migration is saved with the marker and survives a reload.
  appSettings.trackView = TRACK_VIEW_4X2;
  REQUIRE(settingsSave() == 0);
  appSettings.trackView = TRACK_VIEW_ALL;
  REQUIRE(settingsLoad() == 0);
  CHECK(appSettings.trackView == TRACK_VIEW_4X2);
}
#endif

#ifdef RGNANO_BUILD
TEST_CASE_FIXTURE(PagingFixture, "shift map overlay: set on Shift down, cleared on up, only on 1:1 screens") {
  gfxSetPresentUnsquashedCols(GRID_SCREEN_COLS);
  CHECK_FALSE(screenShiftMapActive());
  padKey('k', true);
  CHECK(screenShiftMapActive());
  padKey('k', false);
  CHECK_FALSE(screenShiftMapActive());
  gfxSetPresentUnsquashedCols(0);   // squashed screen already has the sidebar map
  padKey('k', true);
  CHECK_FALSE(screenShiftMapActive());
  padKey('k', false);
}

TEST_CASE_FIXTURE(PagingFixture, "shift map overlay: Shift chords still dispatch and the overlay stays up") {
  gfxSetPresentUnsquashedCols(GRID_SCREEN_COLS);
  mockPageMoves = 0;
  padKey('k', true);
  padKey('n', true);
  CHECK(mockPageMoves == 1);
  CHECK(screenShiftMapActive());
  padKey('n', false);
  CHECK(screenShiftMapActive());   // still held
  padKey('k', false);
  CHECK_FALSE(screenShiftMapActive());
  gfxSetPresentUnsquashedCols(0);
}

TEST_CASE("shift map overlay: box fits in 240x240 and clear of the default cursor") {
  CHECK(SHIFT_MAP_X >= 0);
  CHECK((SHIFT_MAP_X + SHIFT_MAP_W) * 8 == 240);
  CHECK((SHIFT_MAP_Y + SHIFT_MAP_H) * 8 <= 240);
  // Default cursor is the top-left cell of the grid (row 0..), far from rows 14-19 x cols 24-29.
  int cursorCol = 3, cursorRow = 2;
  bool overlaps = cursorCol >= SHIFT_MAP_X && cursorCol < SHIFT_MAP_X + SHIFT_MAP_W &&
                  cursorRow >= SHIFT_MAP_Y && cursorRow < SHIFT_MAP_Y + SHIFT_MAP_H;
  CHECK_FALSE(overlaps);
}
#endif

// TABLE and MODULATION present 1:1 like SONG, CHAIN and PHRASE (gridCols is set in
// the real screen structs, which the unit-test build replaces with mocks).
// Instrument and AY wavetable stay squashed (34 columns: 32 step editor / preview).
// The screens are mocked in the unit-test build, so the layout is checked
// through the shared column maps (the screens also static_assert them).
TEST_CASE("grid screens: widest TABLE row fits 30 columns, columns do not overlap") {
  int right = 0;
  for (int c = 0; c < TABLE_COLUMNS; ++c) {
    right = std::max(right, screenTableColumnX(c) + screenTableColumnW(c));
    if (c > 0) CHECK(screenTableColumnX(c) >= screenTableColumnX(c - 1) + screenTableColumnW(c - 1));
  }
  CHECK(right == 30);
  for (int m = 0; m < 4; ++m) {
    right = std::max(right, screenTableMarkerX(m) + 1);
    // marker cell is free: no column covers it
    for (int c = 0; c < TABLE_COLUMNS; ++c)
      CHECK((screenTableMarkerX(m) < screenTableColumnX(c) || screenTableMarkerX(m) >= screenTableColumnX(c) + screenTableColumnW(c)));
  }
  CHECK(right <= GRID_SCREEN_COLS);
  CHECK(screenTableColumnX(0) >= 1);   // row number sits in column 0
}

TEST_CASE("grid screens: widest MODULATION row fits 30 columns") {
  CHECK(MOD_COL_RIGHT_VAL + MOD_TYPE_W <= GRID_SCREEN_COLS);
  CHECK(MOD_COL_RIGHT_VAL + MOD_DEST_W_RIGHT <= GRID_SCREEN_COLS);
  CHECK(MOD_COL_RIGHT_VAL + MOD_VALUE_W <= GRID_SCREEN_COLS);
  CHECK(MOD_COL_LEFT_VAL + MOD_DEST_W_LEFT <= MOD_COL_RIGHT_X);
  CHECK(MOD_COL_LEFT_VAL + MOD_TYPE_W + 1 <= MOD_COL_RIGHT_X);
}

#ifdef RGNANO_BUILD
// The Shift overlay keys off the presented column count, so any screen with
// gridCols = 30 (TABLE, MODULATION included) gets it; a screen with an FX
// picker open is squashed and carries the sidebar map instead.
TEST_CASE_FIXTURE(PagingFixture, "grid screens: Shift map follows gridCols for converted screens") {
  AppScreen converted = {};
  converted.gridCols = GRID_SCREEN_COLS;
  gfxSetPresentUnsquashedCols(screenPresentCols(&converted, 0));
  padKey('k', true);
  CHECK(screenShiftMapActive());
  padKey('k', false);
  gfxSetPresentUnsquashedCols(screenPresentCols(&converted, 1));   // picker open
  padKey('k', true);
  CHECK_FALSE(screenShiftMapActive());
  padKey('k', false);
  gfxSetPresentUnsquashedCols(0);
}
#endif
