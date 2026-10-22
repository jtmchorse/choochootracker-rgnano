#include "screens.h"
#include <string.h>
#include "corelib_gfx.h"

static char confirmMessage[128];
static int selectedOption = 0; // 0 = OK, 1 = Cancel
static void (*onConfirm)(void);
static void (*onCancel)(void);

void confirmSetup(const char* message, void (*confirmCallback)(void), void (*cancelCallback)(void)) {
  strncpy(confirmMessage, message, 127);
  confirmMessage[127] = 0;
  selectedOption = 0;
  onConfirm = confirmCallback;
  onCancel = cancelCallback;
}

static void setup(int input) {
}

static void fullRedraw(void) {
  gfxSetBgColor(appSettings.colorScheme.background);
  gfxClear();

  gfxSetFgColor(appSettings.colorScheme.selection);
#ifdef RGNANO_BUILD
  // 30 columns: wrap the message at the last space that fits, onto rows 8 and 9.
  {
    const int len = (int)strlen(confirmMessage);
    int cut = len <= 30 ? len : 30;
    if (len > 30) {
      while (cut > 0 && confirmMessage[cut] != ' ') --cut;
      if (cut == 0) cut = 30;
    }
    gfxPrintf(0, 8, "%.*s", cut, confirmMessage);
    if (cut < len) gfxPrintf(0, 9, "%.30s", confirmMessage + cut + (confirmMessage[cut] == ' ' ? 1 : 0));
  }
#else
  gfxPrint(0, 8, confirmMessage);
#endif

  // Draw OK button
  if (selectedOption == 0) {
    gfxSetFgColor(appSettings.colorScheme.textValue);
    gfxPrint(0, 10, "> OK");
  } else {
    gfxSetFgColor(appSettings.colorScheme.textDefault);
    gfxPrint(0, 10, "  OK");
  }

  // Draw Cancel button
  if (selectedOption == 1) {
    gfxSetFgColor(appSettings.colorScheme.textValue);
    gfxPrint(6, 10, "> Cancel");
  } else {
    gfxSetFgColor(appSettings.colorScheme.textDefault);
    gfxPrint(6, 10, "  Cancel");
  }
}

static void draw(void) {
}

static int onInput(int isKeyDown, int keys, int tapCount) {
  if (keys == keyLeft && selectedOption > 0) {
    selectedOption--;
    fullRedraw();
    return 1;
  } else if (keys == keyRight && selectedOption < 1) {
    selectedOption++;
    fullRedraw();
    return 1;
  } else if (keys == keyEdit) {
    if (selectedOption == 0 && onConfirm) {
      onConfirm();
    } else if (selectedOption == 1 && onCancel) {
      onCancel();
    }
    return 1;
  } else if (keys == keyOpt && onCancel) {
    onCancel();
    return 1;
  }
  return 0;
}

const AppScreen screenConfirm = {
  .init = NULL,
  .setup = setup,
  .fullRedraw = fullRedraw,
  .draw = draw,
  .onInput = onInput,
  .getPlaybackLevel = NULL,
};
