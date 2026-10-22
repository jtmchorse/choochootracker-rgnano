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

// INSTRUMENT family (30 wide, 1:1). The common header, ADSR row and filter block are
// shared by every chip page; chip pages keep their own columns inside the same 30.
#define INSTR_LOAD_X        20  // "Load" button (4)
#define INSTR_SAVE_X        25  // "Save" button (4)
#define INSTR_VP_LABEL_X    24  // PAN / VOL labels (3)
#define INSTR_VP_VAL_X      28  // PAN / VOL hex values (2)
#define INSTR_SHP_LABEL_X   25  // ADSR row "SHP" label (3)
#define INSTR_SHP_VAL_X     28  // ADSR row shape value (2)
#define INSTR_FILTER_X      17  // FILTER block labels (<= 6)
#define INSTR_FILTER_VAL_X  23  // FILTER block values
#define INSTR_FILTER_VAL_W  7   // widest filter value ("Classic", "20000Hz")
#define INSTR_PREVIEW_COLS  24  // live / static waveform preview bitmap width
#define INSTR_MODEL_NAME_W  15  // "NN name" model rows start at 11/12, name clipped to 15
#define INSTR_CLIP_NAME(len) ((len) > INSTR_MODEL_NAME_W ? INSTR_MODEL_NAME_W : (int)(len))
#define INSTR_TEXT_COLS     30  // full-width text rows (SoundFont status, errors)
#define INSTR_EDIT_X        26  // SoundFont "Edit" button (4)
// PROJECT (30 wide, 1:1): button row x = Load 0 / Save 5 / New 10 / Export 14 / Manage 21; text
// fields (File, Title, Author, up to 24 characters) start at 6; value column 13 (17 wide);
// the Link connect toggle starts at 6 with its status text at 19.
#define PROJECT_BUTTON_COUNT 5
#define PROJECT_TEXT_X       6
#define PROJECT_VALUE_X      13
#define PROJECT_VALUE_W      17
#define PROJECT_LINK_X       6
#define PROJECT_LINK_STATUS_X 19
int screenProjectButtonX(int button);
int screenProjectButtonW(int button);
// MONK pages: second parameter column label 14 / value 27; common row VOL 16/20, PAN 24/28.
#define MONK_COL2_X         14
#define MONK_COL2_VAL       27
#define MONK_PRESET_W       17  // preset name at 8, EDIT button at 26
#define MONK_EDIT_X         26
#define MONK_VOL_LABEL_X    16
#define MONK_PAN_LABEL_X    24
#define MONK_VOWEL_X        14  // vowel marker letters every 3 columns
#define INSTR_PRESET_NAME_W 21  // preset / collection names printed at x=9
#define INSTR_FB_X          14  // FM "Feedback" label (8)
#define INSTR_FB_VAL_X      23  // FM feedback value ("Preset", 6)
// AY WAVETABLE (30 wide, 1:1): index in 0-1, previews and the 2 x 16 step editor from 2.
#define AYWT_PREVIEW_COLS   28
#define AYWT_STEPS_PER_LINE 16
#define AYWT_EDITOR_Y       11
#define AYWT_EDITOR_LINES   2
int screenAYWavetableStepX(int step);
int screenAYWavetableStepY(int step);

class ScreenOverlayCoordinates {
 public:
  ScreenOverlayCoordinates();
  ~ScreenOverlayCoordinates();
 private:
  int previous_;
  int previousPx_;
  int previousExtra_;
};
