#include "screen_instrument.h"
#include "corelib_gfx.h"
#include "utils.h"
#include "model_catalog.h"
#include "selection_popup.h"
#include "waveform_display.h"
#include "synth/multimode_filter.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

// 1:1 grid layout (30 columns): oscillator values at 8, 303 filter labels at 15, values at 23.
#define ACH_LEFT_VAL 8
#define ACH_FILTER_X 15
#define ACH_FILTER_VAL 23
#define ACH_FILTER_VAL_W 7  // "20000Hz"
#define ACH_MODEL_W 5       // "NN name" clipped: 8 + 3 + 5 = 16 < ACH_FILTER_X + 1
static_assert(ACH_FILTER_VAL + ACH_FILTER_VAL_W <= 30, "AChChid filter values must fit the 1:1 grid");
static_assert(ACH_LEFT_VAL + 3 + ACH_MODEL_W <= ACH_FILTER_X + 1, "AChChid model name must clear the filter labels");
static int modelButtonDown;
static Bitmap* previewBitmap;
static InstrumentAChChid* instrument() { return &chipnomadState->project.instruments[cInstrument].chip.achchid; }
static int isBraids() { return instrument()->wave == AChChidWave::braids; }
static int editLogarithmic16(CellEditAction action, uint16_t* value, uint16_t min, uint16_t max) {
  uint16_t bounded = *value < min ? min : *value > max ? max : *value;
  float ratio = (float)max / min;
  uint8_t control = (uint8_t)(255.0f * logf((float)bounded / min) / logf(ratio) + .5f);
  int handled = edit8noLast(action, &control, 16, 0, 255);
  if (handled) *value = (uint16_t)(min * powf(ratio, control / 255.0f) + .5f);
  return handled;
}
static void drawPreview(const InstrumentAChChid* a) {
  if (!previewBitmap) previewBitmap = gfxBitmapCreate(INSTR_PREVIEW_COLS, 3);
  renderAChChidPreview(previewBitmap, a);
  gfxSetFgColor(appSettings.colorScheme.textInfo);
  gfxDrawBitmap(previewBitmap, 0, 16);
}
static void selectModel(int value) { instrument()->model = (uint8_t)value; projectModified = 1; screenSetup(&screenInstrument, cInstrument); }
static void cancelModel() { screenSetup(&screenInstrument, cInstrument); }
static void openModel() { selectionPopupSetup("BRAIDS MODEL", braidsCategories, braidsCategoryCount, instrument()->model, selectModel, cancelModel); screenSetup(&screenSelectionPopup, 0); }
static int columns(int row) { return row < 3 ? instrumentCommonColumnCount(row) : 2; }
static int isCellValid(int col,int row) {
  if (row < 3 || col) return 1;
  return row <= (isBraids() ? 7 : 4);
}
static void drawStatic() {
  instrumentCommonDrawStatic();
  gfxSetFgColor(appSettings.colorScheme.textTitles); gfxPrint(0,6,"OSCILLATOR"); gfxPrint(ACH_FILTER_X,6,"303 FILTER");
  gfxSetFgColor(appSettings.colorScheme.textDefault); gfxPrint(0,7,"Wave");
  if (isBraids()) { gfxPrint(0,8,"Model"); gfxPrint(0,9,"Timbre"); gfxPrint(0,10,"Color"); gfxPrint(0,11,"Shaper"); } else gfxPrint(0,8,"Fine");
  gfxPrint(ACH_FILTER_X,7,"Cutoff"); gfxPrint(ACH_FILTER_X,8,"Reso"); gfxPrint(ACH_FILTER_X,9,"Env Mod"); gfxPrint(ACH_FILTER_X,10,"Decay"); gfxPrint(ACH_FILTER_X,11,"Accent");
  drawPreview(instrument());
}
// Everything stays inside the 30 column grid.
static void drawCursor(int col,int row) {
  if (row < 3) return instrumentCommonDrawCursor(col, row);
  InstrumentAChChid* a = instrument();
  if (!col) {
    if (row == 3) gfxCursor(ACH_LEFT_VAL, 7, strlen(a->wave == AChChidWave::square ? "Square" : a->wave == AChChidWave::saw ? "Saw" : "Braids"));
    else if (row == 4) gfxCursor(ACH_LEFT_VAL, 8, isBraids() ? 3 + min((int)strlen(modelCatalogName(InstrumentType::Braids, a->model)), ACH_MODEL_W) : 3);
    else if (row >= 5 && row <= 7) gfxCursor(ACH_LEFT_VAL, row + 4, 2);
  } else {
    char value[12];
    snprintf(value, sizeof(value), "%uHz", a->cutoff);
    int width = row == 3 ? (int)strlen(value) : row == 6 ? 6 : 2;
    gfxCursor(ACH_FILTER_VAL, row + 4, width);
  }
}
static void drawField(int col,int row,CellState state) {
  if(row<3) return instrumentCommonDrawField(col,row,state);
  InstrumentAChChid* a=instrument();
  gfxSetFgColor(state==CellState::focus?appSettings.colorScheme.textValue:appSettings.colorScheme.textDefault);
  if(!col && row==3) gfxPrint(ACH_LEFT_VAL,7,a->wave==AChChidWave::square?"Square":a->wave==AChChidWave::saw?"Saw":"Braids");
  else if(!isBraids() && row==4 && !col) gfxPrintf(ACH_LEFT_VAL,8,"%+03d",a->fineTune);
  else if(isBraids() && row==4 && !col) gfxPrintf(ACH_LEFT_VAL,8,"%02u %.5s",a->model,modelCatalogName(InstrumentType::Braids,a->model));
  else if(isBraids() && row==5 && !col) gfxPrint(ACH_LEFT_VAL,9,byteToHex(controlFromRange(a->timbre,32767)));
  else if(isBraids() && row==6 && !col) gfxPrint(ACH_LEFT_VAL,10,byteToHex(controlFromRange(a->color,32767)));
  else if(isBraids() && row==7 && !col) gfxPrint(ACH_LEFT_VAL,11,byteToHex(a->saturation));
  else if(col && row==3) gfxPrintf(ACH_FILTER_VAL,7,"%uHz",a->cutoff);
  else if(col && row==4) gfxPrint(ACH_FILTER_VAL,8,byteToHex(controlFromRange(a->resonance,100)));
  else if(col && row==5) gfxPrint(ACH_FILTER_VAL,9,byteToHex(controlFromRange(a->envMod,100)));
  else if(col && row==6) gfxPrintf(ACH_FILTER_VAL,10,"%4ums",a->decay);
  else if(col && row==7) gfxPrint(ACH_FILTER_VAL,11,byteToHex(controlFromRange(a->accent,100)));
}
static int onEdit(int col,int row,CellEditAction action) {
  if(row<3) return instrumentCommonOnEdit(col,row,action);
  InstrumentAChChid* a=instrument(); int ok=0;
  if(row==3 && !col) ok=edit8noLast(action,(uint8_t*)&a->wave,1,0,2);
  else if(!isBraids() && row==4 && !col) ok=editSigned8(action,&a->fineTune,1,-99,99);
  else if(isBraids() && row==4 && !col) ok=edit8noLast(action,&a->model,1,0,46);
  else if(isBraids() && row==5 && !col) ok=editOscillatorParameter(action,&a->timbre);
  else if(isBraids() && row==6 && !col) ok=editOscillatorParameter(action,&a->color);
  else if(isBraids() && row==7 && !col) ok=edit8noLast(action,&a->saturation,16,0,255);
  else if(col && row==3) ok=editLogarithmic16(action,&a->cutoff,200,FILTER_CUTOFF_MAX_HZ);
  else if(col && row==4) ok=edit8noLast(action,&a->resonance,6,0,100);
  else if(col && row==5) ok=edit8noLast(action,&a->envMod,6,0,100);
  else if(col && row==6) ok=editLogarithmic16(action,&a->decay,200,2000);
  else if(col && row==7) ok=edit8noLast(action,&a->accent,6,0,100);
  if(ok){projectModified=1;screenFullRedraw(&screenInstrumentAChChid);} return ok;
}
static int onInput(int down,int keys,int taps) {
  if(screenInstrumentAChChid.cursorRow!=4||screenInstrumentAChChid.cursorCol||!isBraids()){modelButtonDown=0;return 0;}
  PopupEditInput input=popupEditInput(down,keys,&modelButtonDown);
  if(input==PopupEditInput::cycle) {
    cycle8(&instrument()->model,keys==(keyEdit|keyRight)?1:-1,0,46,0);
    projectModified=1; screenFullRedraw(&screenInstrumentAChChid); return 1;
  }
  if(input==PopupEditInput::hold) return 1;
  if(input==PopupEditInput::open){openModel();return 1;}
  return 0;
}
ScreenData screenInstrumentAChChid={.rows=8,.cursorRow=0,.cursorCol=0,.topRow=0,.selectMode=-1,.playbackLevel=ScreenPlaybackLevel::none,.getColumnCount=columns,.drawStatic=drawStatic,.drawCursor=drawCursor,.drawSelection=NULL,.drawRowHeader=NULL,.drawColHeader=NULL,.drawField=drawField,.onEdit=onEdit,.onInput=onInput,.onRawInput=NULL,.isCellValid=isCellValid,.getLoopRange=NULL};
