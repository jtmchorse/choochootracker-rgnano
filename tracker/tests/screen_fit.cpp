// Developer tool: draw every production screen against the recording gfx mock and
// report the rightmost cell column touched. RG Nano shows 30 columns (0-29).
#include "common.h"
#include "app.h"
#include "screens.h"
#include "screen_layout.h"
#include "screen_audio_input.h"
#include "corelib_gfx.h"
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "selection_popup.h"
#include "file_browser.h"
#include "screen_enter_name.h"
#include "screen_create_folder.h"
#include "project_utils.h"
#include "project_instruments.h"
#include "help.h"
#include "screen_instrument.h"
#include <cstdlib>
#include <unistd.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/wait.h>

extern int mockMaxRight;

// Stubs for platform pieces the production screens reference but the gfx mock lacks.
#include "corelib_font.h"
#include "corelib_mainloop.h"
#include <cstring>
#include <algorithm>
void mainLoopSetTargetFps(int) {}
int mainLoopGetTargetFps(void) { return 30; }
int mainLoopGetFrameTimeUs(void) { return 0; }
struct GfxImage { int w, h; };
GfxImage* gfxImageLoadBMP(const char*) { return nullptr; }
GfxImage* gfxImageCreateARGB(int w, int h) { return new GfxImage{w, h}; }
int gfxImageUpdateARGB(GfxImage*, const uint32_t*) { return 0; }
void gfxImageFree(GfxImage* i) { delete i; }
int gfxImageWidth(const GfxImage* i) { return i ? i->w : 0; }
int gfxImageHeight(const GfxImage* i) { return i ? i->h : 0; }
void gfxImageDrawCrop(const GfxImage*, int, int, int, int, int, int) {}
void gfxTitleBegin(void) {}
void gfxTitleFadeBlack(uint8_t) {}
void gfxTitlePresent(void) {}
void gfxTitleEnd(void) {}
void gfxTitlePrint(int x, int, const char* text) { mockMaxRight = std::max(mockMaxRight, x + (int)strlen(text) - 1); }

void charEditFullDraw(char startChar);

static void drawScreen(const AppScreen* screen) {
  currentScreen = screen;
  if (screen->setup) screen->setup(0);
  screen->fullRedraw();
  screen->draw();
}

static void setType(InstrumentType type) {
  Instrument* in = &chipnomadState->project.instruments[0];
  InstrumentFunctions old = getInstrumentFunctions(in->type);
  if (old.free) old.free(in);
  in->type = type;
  InstrumentFunctions now = getInstrumentFunctions(type);
  if (now.init) now.init(in);
  cInstrument = 0;
}

struct Scenario { std::string name; std::function<void()> run; bool shiftMap = false; };

int main() {
  initDefaultAppSettings();
  appSetup();
  if (projectLoad(&chipnomadState->project, "packaging/common/projects/grieg-mountain-king-fm.cct")) { printf("project load failed\n"); return 2; }
  screensInitAll();
  std::vector<Scenario> all;
  auto add = [&](const std::string& n, std::function<void()> f) { all.push_back({n, f, false}); };
  const struct { const char* name; const AppScreen* screen; } plain[] = {
    {"Title", &screenTitle}, {"Song", &screenSong}, {"Chain", &screenChain}, {"Phrase", &screenPhrase},
    {"Groove", &screenGroove}, {"InstrumentPool", &screenInstrumentPool},
    {"Table", &screenTable}, {"AYWavetable", &screenAYWavetable}, {"Export", &screenExport}, {"Bounce", &screenBounce},
    {"Manage", &screenManage}, {"Settings", &screenSettings}, {"Controls", &screenControlsSettings},
    {"Audio", &screenAudioSettings}, {"TrackVisuals", &screenTrackVisuals}, {"Synth", &screenSynthSettings},
    {"MixerSettings", &screenMixerSettings}, {"Graphics", &screenGraphicsSettings}, {"Mixer", &screenMixer},
    {"ColorTheme", &screenColorTheme}, {"KeyMapping", &screenKeyMapping}, {"Midi", &screenMidi},
    {"MidiChannelMap", &screenMidiChannelMap}, {"MidiCC", &screenMidiCC}, {"QuickHelp", &screenQuickHelp},
    {"Project", &screenProject}, {"Confirm", &screenConfirm}, {"SaveChoice", &screenSaveChoice},
    {"PitchTable", &screenPitchTable}, {"Scale", &screenScale}, {"AudioInput", &screenAudioInput},
    {"InsertFX", &screenInsertFX},
  };
  for (const auto& n : plain) { const AppScreen* sc = n.screen; add(n.name, [sc] { drawScreen(sc); }); }
  // Shift held: the screen map overlay is drawn bottom right (columns 24-29) on every screen
  // except the title, and nothing else appears right of column 29.
  for (const auto& n : plain) {
    if (n.screen == &screenTitle) continue;
    const AppScreen* sc = n.screen;
    all.push_back({std::string("shiftmap/") + n.name, [sc] { drawScreen(sc); screenSetShiftHeld(1); mockMaxRight = -1; drawScreenMap(); }, true});
  }
  // Every instrument type through the instrument-dependent screens.
  for (int t = 1; t < int(InstrumentType::totalCount); ++t) {
    InstrumentType type = InstrumentType(t);
    if (!getInstrumentDefinition(type)) continue;
    std::string tn = std::string("type") + std::to_string(t) + "/";
    add(tn + "Instrument", [type] { setType(type); drawScreen(&screenInstrument); });
    add(tn + "Modulation", [type] { setType(type); drawScreen(&screenModulation); });
    add(tn + "FXpicker", [type] { setType(type); currentScreen = &screenPhrase; fxEditFullDraw((FXId)0, 0, 0); });
    add(tn + "Table", [type] { setType(type); drawScreen(&screenTable); });
    if (type == InstrumentType::Sample) add(tn + "SampleSettings", [type] { setType(type); drawScreen(&screenSampleSettings); });
    if (type == InstrumentType::SID) add(tn + "SIDEdit", [type] { setType(type); drawScreen(&screenSIDEdit); });
    if (type == InstrumentType::SoundFont) add(tn + "SoundFontEdit", [type] { setType(type); drawScreen(&screenSoundFontEdit); });
    if (type == InstrumentType::Monk) add(tn + "MonkEdit", [type] { setType(type); drawScreen(&screenMonkEdit); });
    if (fmEditSupported(type)) add(tn + "FMEdit", [type] { setType(type); drawScreen(&screenFMEdit); });
  }
  add("CharPicker", [] { charEditFullDraw('A'); });
  add("EnterName", [] { enterNameSetup("A VERY LONG TITLE THAT GOES ON AND ON", "A very long prompt that overruns the width", "abcdefghijklmnopqrstuvwx", nullptr, nullptr); drawScreen(&screenEnterName); });
  add("CreateFolder", [] { createFolderSetup("/a/very/long/path/that/goes/on/and/on/for/a/long/time/indeed/folder", nullptr, nullptr); drawScreen(&screenCreateFolder); });
  add("Confirm long", [] { confirmSetup("This is a long confirmation message that must wrap to two lines", nullptr, nullptr); drawScreen(&screenConfirm); });
  add("SaveChoice long", [] { saveChoiceSetup("a_sample_file_name_that_is_really_quite_long.wav", nullptr, nullptr, nullptr); drawScreen(&screenSaveChoice); });
  add("Export long path", [] {
    snprintf(appSettings.exportPath, sizeof(appSettings.exportPath), "/media/sdcard/some/long/export/folder/name/here");
    drawScreen(&screenExport);
  });
  add("FileBrowser", [] { fileBrowserSetup("OPEN A FILE WITH A LONG TITLE", ".cct", "packaging/common/projects", nullptr, nullptr); drawScreen(&screenFileBrowser); });
  add("FileBrowser folder", [] { fileBrowserSetupFolderMode("SAVE A FILE", "/root/src/ccr-p10-allscreens/tracker/packaging/common/projects/with/an/extra/long/path", "name", ".cct", nullptr, nullptr); drawScreen(&screenFileBrowser); });
  static const SelectionItem kids[] = {{"A child with a very long label indeed", 1, nullptr, 0, "helper text that is long enough to overrun the row", false},
                                       {"Child two", 2, nullptr, 0, nullptr, false}};
  static const SelectionItem cats[] = {{"A category with a long label", 10, kids, 2, nullptr, false},
                                       {"Second", 11, kids, 2, nullptr, true}};
  for (int mode = 0; mode < 4; ++mode) {
    add("Popup mode" + std::to_string(mode), [mode] {
      bool wide = mode == 1, preview = mode == 2, coll = mode == 3;
      selectionPopupSetup("A POPUP TITLE", cats, 2, 1, nullptr, nullptr, wide,
                          preview ? (void (*)(int, bool))[](int, bool) {} : nullptr, coll, coll, nullptr, false);
      drawScreen(&screenSelectionPopup);
    });
  }
  int bad = 0;
  for (const auto& n : all) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      signal(SIGSEGV, [](int) { void* b[20]; int c = backtrace(b, 20); backtrace_symbols_fd(b, c, 1); _exit(139); });
      mockMaxRight = -1;
      n.run();
      const bool failed = n.shiftMap ? mockMaxRight != 29 : mockMaxRight > 29;
      printf("%-22s right=%2d %s\n", n.name.c_str(), mockMaxRight, failed ? "FAIL" : "");
      fflush(stdout);
      _exit(failed ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st)) { printf("%-22s CRASH (signal %d)\n", n.name.c_str(), WTERMSIG(st)); ++bad; }
    else if (WEXITSTATUS(st)) ++bad;
  }
  printf("%d scenarios over 30 columns or crashed\n", bad);
  return bad ? 1 : 0;
}
