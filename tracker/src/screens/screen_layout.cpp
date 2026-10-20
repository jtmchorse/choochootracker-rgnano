#include "screen_layout.h"
#include "screens.h"
#include "corelib_gfx.h"

int screenScopeRows(const AppScreen* screen) {
  return appSettings.persistentWaveform && screen == &screenSong ? 2 : 0;
}

int screenVisibleRows(void) {
  // Modulation has two compact blocks of eight logical fields, not a list.
  return currentScreen == &screenModulation ? 16 : 16 - screenScopeRows(currentScreen);
}

int screenModulationRowY(int row) {
  if (row < 0 || row >= 16) return -1;
  return row < 8 ? row + 2 : row + 3;
}

int screenModulationRowAt(int y) {
  for (int row = 0; row < 16; ++row)
    if (screenModulationRowY(row) == y) return row;
  return -1;
}

ScreenOverlayCoordinates::ScreenOverlayCoordinates()
  : previous_(gfxGetContentRowOffset()), previousPx_(gfxGetPixelOffsetX()), previousExtra_(gfxGetRectExtraWidth()) {
  gfxSetContentRowOffset(0);
  gfxSetPixelOffsetX(0);
  gfxSetRectExtraWidth(0);
}
ScreenOverlayCoordinates::~ScreenOverlayCoordinates() {
  gfxSetContentRowOffset(previous_);
  gfxSetPixelOffsetX(previousPx_);
  gfxSetRectExtraWidth(previousExtra_);
}

// Columns the current screen draws into: grid screens are limited to their
// unsquashed width while presented 1:1, everything else uses the whole canvas.
int screenVisibleCols(void) {
  int cols = gfxGetPresentUnsquashedCols();
  return cols > 0 ? cols : 40;
}

// Columns presented 1:1 for `screen` (0 = squashed 40 column canvas). A grid
// screen falls back to the squashed canvas while something wider than its
// region is drawn over it (character picker, FX picker).
int screenPresentCols(const AppScreen* screen, int wideOverlayOpen) {
  int cols = screen ? screen->gridCols : 0;
  return wideOverlayOpen ? 0 : cols;
}
