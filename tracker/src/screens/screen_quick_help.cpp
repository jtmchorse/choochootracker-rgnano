#include "screen_quick_help.h"
#include "common.h"
#include "corelib_gfx.h"
#include "screens.h"

static const AppScreen* returnScreen;
static int ignoreOpeningRelease;
#ifdef RGNANO_BUILD
static_assert(sizeof("Select+Pad  Change screen") - 1 <= 30 && sizeof("A btn       Scroll entries") - 1 <= 30, "QUICK HELP lines must fit 30 columns");
#endif

void screenQuickHelpOpen(const AppScreen* screen) {
  returnScreen = screen;
  screenSetup(&screenQuickHelp, 0);
}

static void setup(int input) {
  ignoreOpeningRelease = 1;
}

static void fullRedraw(void) {
  gfxSetFgColor(appSettings.colorScheme.textTitles);
  gfxPrint(0, 0, "QUICK HELP");
  gfxSetFgColor(appSettings.colorScheme.textDefault);
#ifdef RGNANO_BUILD
  // 30 column lines
  gfxPrint(0, 2, "Select+Pad  Change screen");
  gfxPrint(0, 3, "A btn       Edit values");
  gfxPrint(0, 4, "B btn       Scroll entries");
  gfxPrint(0, 5, "A + B       Delete value");
#else
  gfxPrint(0, 2, "Select  Gamepad | Navigate Screens");
  gfxPrint(0, 3, "A btn   Gamepad | Edit values");
  gfxPrint(0, 4, "B btn   Gamepad | Scroll entries");
  gfxPrint(0, 5, "A btn   B btn   | Delete value");
#endif
  gfxPrint(0, 7, "A song has chains");
  gfxPrint(0, 8, "Chains have phrases");
  gfxPrint(0, 9, "Phrases play instruments");
  gfxPrint(0, 10, "Instruments make sound");
  gfxPrint(0, 13, "Read the manual next time");
  gfxPrint(0, 14, "you are on a train.");

}

static void draw(void) {}

static int onInput(int isKeyDown, int keys, int tapCount) {
  if (!isKeyDown && ignoreOpeningRelease) {
    ignoreOpeningRelease = 0;
    return 1;
  }
  if (isKeyDown) screenSetup(returnScreen, 0);
  return 1;
}

const AppScreen screenQuickHelp = {
  .init = NULL,
  .setup = setup,
  .fullRedraw = fullRedraw,
  .draw = draw,
  .onInput = onInput,
  .getPlaybackLevel = NULL,
};
