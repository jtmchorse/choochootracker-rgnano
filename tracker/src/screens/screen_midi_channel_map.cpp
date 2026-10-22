#include "screen_midi_channel_map.h"
#include "screen_midi.h"
#include "common.h"
#include "corelib_gfx.h"
#include "corelib_input.h"
#include "project_utils.h"
#include "screens.h"
#include "midi/midi_router.h"
#include <string.h>

#ifdef RGNANO_BUILD
// 30 columns: "CH 01" then "XX: name" from column 8 (name up to 15 characters).
#define MCM_VALUE_X 8
static_assert(MCM_VALUE_X + 22 <= 30, "MIDI CHANNEL MAP value column must fit 30 columns");
#else
#define MCM_VALUE_X 23
#endif
static int columnCount(int row) {
  return 1;
}

static void drawStatic(void) {
  gfxSetFgColor(appSettings.colorScheme.textTitles);
  gfxPrint(0, 0, "MIDI CHANNEL MAP");
}

static void drawCursor(int col, int row) {
  gfxCursor(MCM_VALUE_X, 2 + row, 20);
}

static void drawRowHeader(int row, CellState state) {
}

static void drawColHeader(int col, CellState state) {
}

static void drawField(int col, int row, CellState state) {
  int y = 2 + row;
  gfxSetFgColor(appSettings.colorScheme.textDefault);
  gfxPrintf(0, y, "CH %02d", row + 1);

  gfxSetFgColor(state == CellState::focus ? appSettings.colorScheme.textValue : appSettings.colorScheme.textDefault);
  gfxClearRect(MCM_VALUE_X, y, 22, 1);
  int8_t instrument = appSettings.midiChannelInstrument[row];
  if (instrument < 0) {
    gfxPrint(MCM_VALUE_X, y, "OFF");
  } else {
    gfxPrintf(MCM_VALUE_X, y, "%s: %s", byteToHex((uint8_t)instrument), instrumentName(&chipnomadState->project, (uint8_t)instrument));
  }
}

static int onEdit(int col, int row, CellEditAction action) {
  action = convertMultiAction(action);
  int direction = (action == CellEditAction::increase || action == CellEditAction::increaseBig) ? 1 :
                  (action == CellEditAction::decrease || action == CellEditAction::decreaseBig) ? -1 : 0;
  if (!direction) return 0;
  int value = appSettings.midiChannelInstrument[row] + direction;
  if (value < -1) value = PROJECT_MAX_INSTRUMENTS - 1;
  if (value >= PROJECT_MAX_INSTRUMENTS) value = -1;
  appSettings.midiChannelInstrument[row] = (int8_t)value;
  midiRouterSetChannelInstrumentMap(chipnomadState->midiRouter, appSettings.midiChannelInstrument);
  return 1;
}

static ScreenData screenMidiChannelMapData = {
  .rows = MIDI_CHANNEL_COUNT,
  .cursorRow = 0,
  .cursorCol = 0,
  .topRow = 0,
  .selectMode = -1,
  .selectStartRow = 0,
  .selectStartCol = 0,
  .selectAnchorRow = 0,
  .selectAnchorCol = 0,
  .playbackLevel = ScreenPlaybackLevel::none,
  .getColumnCount = columnCount,
  .drawStatic = drawStatic,
  .drawCursor = drawCursor,
  .drawSelection = NULL,
  .drawRowHeader = drawRowHeader,
  .drawColHeader = drawColHeader,
  .drawField = drawField,
  .onEdit = onEdit,
  .onInput = NULL,
  .onRawInput = NULL,
  .isCellValid = NULL,
  .getLoopRange = NULL,
};

static void setup(int input) {
  screenMidiChannelMapData.cursorRow = 0;
  screenMidiChannelMapData.cursorCol = 0;
}

static void fullRedraw(void) {
  screenFullRedraw(&screenMidiChannelMapData);
}

static void draw(void) {
}

static int inputScreenNavigation(int keys, int tapCount) {
  if (keys == keyOpt) {
    screenSetup(&screenMidi, 0);
    return 1;
  }
  return 0;
}

static int onInput(int isKeyDown, int keys, int tapCount) {
  if (inputScreenNavigation(keys, tapCount)) return 1;
  return screenInput(&screenMidiChannelMapData, isKeyDown, keys, tapCount);
}

const AppScreen screenMidiChannelMap = {
  .init = NULL,
  .setup = setup,
  .fullRedraw = fullRedraw,
  .draw = draw,
  .onInput = onInput,
  .getPlaybackLevel = NULL,
};
