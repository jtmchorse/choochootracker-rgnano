#include "selection_popup.h"

#include "corelib_gfx.h"
#include "chipnomad_lib.h"
#include <string.h>
#include <chrono>
#include <algorithm>
#include <string>
#include <vector>

// Panel geometry. RG Nano draws 30 columns: a 12 column category panel (">" + label),
// a divider at 12 and a 17 column item panel; other builds keep the 40 column layout.
#ifdef RGNANO_BUILD
#define SP_TEXT_W 30          // status / path / footer line width
#define SP_SEP_X 13           // "|" divider
#define SP_RIGHT_X 14         // item panel origin
#define SP_LEFT_LABEL_W 11    // category label after ">" and a space
#define SP_RIGHT_LABEL_W 13   // item label after ">*" and a space
#define SP_FULL_LABEL_W 28    // single panel label after ">" and a space
#define SP_C_SEP_X 12         // collection layout divider
#define SP_C_RIGHT_X 13       // collection layout item panel origin
#define SP_C_LEFT_LABEL_W 8   // collection layout category label (after ">", icon)
#define SP_C_PRESET_ICON_X 13 // "PRESET" tab icon, text two columns later
#define SP_C_RIGHT_LABEL_W(y) 13
#else
#define SP_TEXT_W 40
#define SP_SEP_X 18
#define SP_RIGHT_X 19
#define SP_C_SEP_X 18
#define SP_C_RIGHT_X 19
#define SP_LEFT_LABEL_W 16
#define SP_RIGHT_LABEL_W 18
#define SP_FULL_LABEL_W 38
#define SP_C_LEFT_LABEL_W 14
#define SP_C_PRESET_ICON_X 20
#define SP_C_RIGHT_LABEL_W(y) (((y>=3 && y<=10) || y>=14) ? 11 : 17)
#endif
static_assert(SP_RIGHT_X + 3 + SP_RIGHT_LABEL_W <= SP_TEXT_W, "selection popup item panel must fit");
static_assert(SP_C_RIGHT_X + 4 + 13 <= 30 || SP_C_RIGHT_X == 19, "selection popup collection panel must fit");

static char title[32];
static const SelectionItem* rootItems;
static bool fullWidth, collectionLayout, folderNavigation;
static const char* (*onStatus)();
static std::string statusText;
static void drawIcon(bool folder, int col, int row) {
  const int w=gfxGetCharWidth(), h=gfxGetCharHeight();
  std::vector<uint8_t> pixels(size_t(w)*h);
  const uint8_t folderRows[8]={0,0x70,0x7e,0x42,0x42,0x42,0x7e,0};
  const uint8_t fileRows[8]={0,0x7e,0,0x7e,0,0x7e,0,0};
  const auto* rows=folder?folderRows:fileRows;
  for(int y=0;y<h;++y)for(int x=0;x<w;++x)
    pixels[size_t(y)*w+x]=(rows[y*8/h] & (0x80>>(x*8/w)))?255:0;
  gfxDrawCharBitmap(pixels.data(),col,row);
}
static int rootCount, categoryIndex, itemIndex, activePanel, currentValue;
static void (*onSelected)(int);
static void (*onCancelled)(void);
static void (*onPreview)(int, bool);
static bool previewHeld, editPending;
static std::chrono::steady_clock::time_point selectionSince;
static size_t marqueeOffset;
static void resetMarquee() { selectionSince=std::chrono::steady_clock::now();marqueeOffset=0; }

static const SelectionItem* currentCategory() {
  return &rootItems[categoryIndex];
}

static void selectCurrentValue() {
  categoryIndex = itemIndex = 0;
  for (int i = 0; i < rootCount; ++i) {
    const SelectionItem* category = &rootItems[i];
    if (category->value == currentValue) {
      categoryIndex = i;
      return;
    }
    for (int j = 0; j < category->childCount; ++j) {
      if (category->children[j].value == currentValue) {
        categoryIndex = i;
        itemIndex = j;
        return;
      }
    }
  }
}

void selectionPopupSetup(const char* popupTitle, const SelectionItem* items,
                         int count, int selectedValue,
                         void (*selected)(int), void (*cancelled)(void), bool wide, void (*preview)(int, bool), bool collections, bool focusPreset, const char* (*loadingStatus)(), bool folders) {
  onStatus=loadingStatus; folderNavigation=folders; statusText.clear();
  collectionLayout=collections;
  resetMarquee();
  onPreview = preview; previewHeld = editPending = false;
  fullWidth = wide;
  strncpy(title, popupTitle, sizeof(title) - 1);
  title[sizeof(title) - 1] = 0;
  rootItems = items;
  rootCount = count;
  currentValue = selectedValue;
  activePanel = 0;
  onSelected = selected;
  onCancelled = cancelled;
  selectCurrentValue();
  if(focusPreset && rootCount>0 && currentCategory()->childCount &&
     (!folderNavigation || currentCategory()->value!=currentValue))activePanel=1;
}

bool selectionPopupIsFullWidth(void) { return fullWidth; }

static void setup(int input) {}

static void fullRedraw() {
  gfxClear();
  gfxSetFgColor(appSettings.colorScheme.textTitles);
  gfxPrint(0, 0, title);
  if (!fullWidth) gfxPrint(collectionLayout ? SP_C_SEP_X : SP_SEP_X, 1, "|");
  if(collectionLayout){drawIcon(true,0,1);gfxPrint(2,1,"COLLECTION");drawIcon(false,SP_C_PRESET_ICON_X,1);gfxPrint(SP_C_PRESET_ICON_X+2,1,"PRESET");}
  if (onStatus) {
    statusText=onStatus();
    gfxSetFgColor(appSettings.colorScheme.textInfo);
    gfxPrintf(0,2,"%.*s",SP_TEXT_W,statusText.c_str());
  }
  const SelectionItem* category = currentCategory();
  // Eight track previews occupy rows 3..10 on the SoundFont screen. Keep the
  // selected name on row 11, where it can use the full width of its panel.
  int categoryStart = collectionLayout ? std::max(0,categoryIndex-14) : std::max(0,categoryIndex-7);
  int itemStart = collectionLayout ? itemIndex - 8 : std::max(0,itemIndex-7);
  for (int row = 0; row < (collectionLayout ? 15 : 16); ++row) {
    int categoryItem = categoryStart + row;
    int childItem = itemStart + row;
    int y = (collectionLayout ? 3 : 2) + row;
    if (categoryItem >= 0 && categoryItem < rootCount) {
      gfxSetFgColor(categoryItem == categoryIndex && activePanel == 0 ?
        appSettings.colorScheme.textValue : appSettings.colorScheme.textDefault);
      const size_t offset=collectionLayout && categoryItem==categoryIndex && activePanel==0 ? marqueeOffset : 0;
      if(collectionLayout) {
        gfxPrintf(0,y,"%c",categoryItem==categoryIndex?'>':' ');
        drawIcon(true,2,y);
        gfxPrintf(4,y,"%-*.*s",SP_C_LEFT_LABEL_W,SP_C_LEFT_LABEL_W,rootItems[categoryItem].label+offset);
      } else {
        const int labelW = fullWidth ? SP_FULL_LABEL_W : SP_LEFT_LABEL_W;
        gfxPrintf(0,y,"%c %-*.*s",
          categoryItem==categoryIndex?'>':' ',labelW,labelW,rootItems[categoryItem].label);
      }
    }
    if (childItem >= 0 && childItem < category->childCount) {
      const SelectionItem* item = &category->children[childItem];
      gfxSetFgColor(childItem == itemIndex && activePanel == 1 ?
        appSettings.colorScheme.textValue : appSettings.colorScheme.textDefault);
      const int width=collectionLayout ? SP_C_RIGHT_LABEL_W(y) : SP_RIGHT_LABEL_W;
      const size_t offset=collectionLayout && childItem==itemIndex && activePanel==1 ? marqueeOffset : 0;
      if(collectionLayout) {
        gfxPrintf(SP_C_RIGHT_X,y,"%c%c",childItem==itemIndex?'>':' ',item->value==currentValue?'*':' ');
        drawIcon(item->folder,SP_C_RIGHT_X+2,y);
        gfxPrintf(SP_C_RIGHT_X+4,y,"%-*.*s",width,width,item->label+offset);
      } else {
        gfxPrintf(SP_RIGHT_X,y,"%c%c %-*.*s",childItem==itemIndex?'>':' ',
          item->value==currentValue?'*':' ',width,width,item->label+offset);
      }
    }
  }
  if(collectionLayout&&category->helper){gfxSetFgColor(appSettings.colorScheme.textInfo);const char*path=category->helper;size_t length=strlen(path);gfxPrintf(0,18,"%-*.*s",SP_TEXT_W,SP_TEXT_W,length>(size_t)SP_TEXT_W?path+length-SP_TEXT_W:path);}
  else if (activePanel == 1 && itemIndex < category->childCount && category->children[itemIndex].helper) {
    gfxSetFgColor(appSettings.colorScheme.textInfo);
    gfxPrintf(0, 18, "%-*.*s", SP_TEXT_W, SP_TEXT_W, category->children[itemIndex].helper);
  }
  gfxSetFgColor(appSettings.colorScheme.textInfo);
  const bool playing = chipnomadState && chipnomadGetPlaybackStatus(chipnomadState)->isPlaying;
#ifdef RGNANO_BUILD
  if(!collectionLayout) gfxPrint(0, 19, onPreview && playing ? "STOP SONG TO HEAR  OPT EXIT" :
    folderNavigation ? "L BACK R PAGE EDIT OPEN" :
    onPreview ? "EDIT PICK +PLAY HEAR OPT EXIT" :
    fullWidth ? "U/D MOVE EDIT SELECT OPT EXIT" :
    "L/R PANEL EDIT PICK OPT EXIT");
#else
  if(!collectionLayout) gfxPrint(0, 19, onPreview && playing ? "STOP SONG TO HEAR  EDIT SELECT  OPT EXIT" :
    folderNavigation ? "L BACK R PAGE EDIT OPEN PLAY SONG" :
    collectionLayout ? "L/R PANEL  EDIT+PLAY HEAR  OPT EXIT" :
    onPreview ? "EDIT SELECT  EDIT+PLAY HEAR  OPT EXIT" :
    fullWidth ? "U/D MOVE EDIT SELECT OPT EXIT" :
    "L/R PANEL U/D MOVE EDIT SELECT OPT EXIT");
#endif
}

static void draw() {
  if(!collectionLayout || rootCount<=0)return;
  const auto* category=currentCategory();
  const char* label=activePanel==1 && category->childCount ? category->children[itemIndex].label : category->label;
  const size_t width=activePanel==1?(size_t)SP_C_RIGHT_LABEL_W(0):(size_t)SP_C_LEFT_LABEL_W, length=strlen(label);
  size_t offset=0;
  if(length>width) {
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-selectionSince).count();
    const auto travel=int64_t(length-width)*180;
    const auto phase=elapsed%(1000+travel+1000);
    if(phase>=1000)offset=size_t(std::min<int64_t>(length-width,(phase-1000)/180+1));
  }
  const bool statusChanged=onStatus && statusText!=onStatus();
  if(offset!=marqueeOffset || statusChanged){marqueeOffset=offset;fullRedraw();}
}

static int onInput(int isKeyDown, int keys, int tapCount) {
  if (collectionLayout && (keys == keyPlay || keys == (keyShift | keyPlay))) return 0;
  if (onPreview && keys == (keyEdit | keyPlay) && isKeyDown) {
    const auto* category = currentCategory();
    int value = activePanel == 1 && category->childCount ? category->children[itemIndex].value : category->value;
    editPending=false;
    if (value >= 0) { onPreview(value, true); previewHeld = true; }
    return 1;
  }
  if (previewHeld) {
    if (keys != (keyEdit | keyPlay)) { onPreview(0, false); previewHeld = false; }
    return 1;
  }
  // Defer EDIT selection until release on audition-capable popups so pressing
  // EDIT before PLAY can form the existing preview chord without committing.
  if(onPreview&&isKeyDown&&keys==keyEdit){editPending=true;return 1;}
  bool selectOnRelease=onPreview&&!isKeyDown&&editPending;
  if(selectOnRelease){editPending=false;keys=keyEdit;}
  if (!isKeyDown&&!selectOnRelease) return 1;
  if (keys == keyUp || keys == keyDown) {
    resetMarquee();
    int direction = keys == keyUp ? -1 : 1;
    if (activePanel == 0) {
      categoryIndex += direction;
      if (categoryIndex < 0) categoryIndex = rootCount - 1;
      if (categoryIndex >= rootCount) categoryIndex = 0;
      itemIndex = 0;
    } else {
      int count = currentCategory()->childCount;
      if (!count) return 1;
      itemIndex += direction;
      if (itemIndex < 0) itemIndex = count - 1;
      if (itemIndex >= count) itemIndex = 0;
    }
    fullRedraw();
    return 1;
  }
  if (keys == keyLeft || keys == keyRight) {
    resetMarquee();
    if(folderNavigation) {
      if(keys==keyLeft && activePanel==0) { if(onCancelled)onCancelled();return 1; }
      if(keys==keyRight && activePanel==0 && currentCategory()->folder) {
        if(onSelected)onSelected(currentCategory()->value);
        return 1;
      }
      if(keys==keyRight && activePanel==1 && currentCategory()->childCount) {
        itemIndex = std::min(itemIndex + 15, currentCategory()->childCount - 1);
        fullRedraw();
        return 1;
      }
    }
    activePanel = keys == keyLeft ? 0 : 1;
    if (!currentCategory()->childCount) activePanel = 0;
    fullRedraw();
    return 1;
  }
  if (keys == keyEdit) {
    if (activePanel == 0) {
      if (folderNavigation && currentCategory()->folder) {
        if(onSelected)onSelected(currentCategory()->value);
      } else if (currentCategory()->childCount) {
        resetMarquee();
        activePanel = 1;
        fullRedraw();
      } else if (onSelected) {
        onSelected(currentCategory()->value);
      }
    } else if (onSelected && currentCategory()->childCount) {
      onSelected(currentCategory()->children[itemIndex].value);
    }
    return 1;
  }
  if (keys == keyOpt) {
    if (onCancelled) {
      onCancelled();
    }
    return 1;
  }
  return 1;
}

static ScreenPlaybackLevel playbackLevel() { return collectionLayout ? ScreenPlaybackLevel::song : ScreenPlaybackLevel::none; }

const AppScreen screenSelectionPopup = {
  .init = NULL, .setup = setup, .fullRedraw = fullRedraw, .draw = draw,
  .onInput = onInput, .getPlaybackLevel = playbackLevel,
};
