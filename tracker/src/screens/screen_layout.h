#pragma once
struct AppScreen;

// The optional Song scope occupies rows 0-1. The sidebar and message row remain in physical
// screen coordinates; only the page's content uses the two-row offset.
int screenScopeRows(const AppScreen* screen);
int screenVisibleRows(void);
// 30 while a grid screen is presented unsquashed, else 40.
int screenVisibleCols(void);
int screenPresentCols(const AppScreen* screen, int wideOverlayOpen);
// Shift-held map overlay for unsquashed grid screens: a 6x6 cell box at the bottom right.
#define SHIFT_MAP_W 6
#define SHIFT_MAP_H 6
#define SHIFT_MAP_Y 14
#define SHIFT_MAP_X (30 - SHIFT_MAP_W)
void screenSetShiftHeld(int held);
int screenShiftMapActive(void); // Shift held and the screen is presented 1:1
// TABLE columns (30 wide, 1:1): flag, pitch, volume, then FX name/value pairs.
// Marker m is the play-marker cell in front of the volume/FX1..FX3 groups.
#define TABLE_COLUMNS 11
int screenTableColumnX(int col);
int screenTableColumnW(int col);
int screenTableMarkerX(int marker); // 0-3
// MODULATION blocks (30 wide, 1:1): left block label 0 / value 7, right block label 16 / value 23.
#define MOD_COL_LEFT_X    0
#define MOD_COL_LEFT_VAL  7
#define MOD_COL_RIGHT_X   16
#define MOD_COL_RIGHT_VAL 23
#define MOD_DEST_W_LEFT   9   // clipped before the right block's label
#define MOD_DEST_W_RIGHT  7
#define MOD_TYPE_W        7   // syncLFO / slowLFO
#define MOD_VALUE_W       6   // widest parameter value (LFO shape, "Normal")
// Modulation uses two blocks of eight fields, with a gap between them.
int screenModulationRowY(int row);
int screenModulationRowAt(int y);

class ScreenOverlayCoordinates {
 public:
  ScreenOverlayCoordinates();
  ~ScreenOverlayCoordinates();
 private:
  int previous_;
  int previousPx_;
  int previousExtra_;
};
