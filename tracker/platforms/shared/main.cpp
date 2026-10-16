#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "corelib_gfx.h"
#include "corelib_font.h"
#include "corelib_mainloop.h"
#include "app.h"
#include "common.h"
#ifdef RGNANO_BUILD
#include "rgnano_platform.h"
#endif

int main(int argv, char** args) {
#ifdef RGNANO_BUILD
  // Data dir, log, first-run asset seeding and the audio open all happen
  // before settings, video or evdev are touched.
  rgnanoPlatformEarlyInit();
#endif
  settingsLoad();

  // Load custom font before gfxSetup so it uses the correct font
  if (appSettings.fontPath[0] != '\0') {
    Font* font = fontLoad(appSettings.fontPath);
    if (font) {
      fontSetCurrent(font);
    } else {
      appSettings.fontPath[0] = '\0';
      fontSetCurrent(NULL);
    }
  }

  if (gfxSetup(&appSettings.screenWidth, &appSettings.screenHeight) != 0) return 1;

  appSetup();
  mainLoopRun(appDraw, appOnEvent);
#ifndef WEB_BUILD
  appCleanup();
  gfxCleanup();
  mainLoopQuit();
#ifdef RGNANO_BUILD
  rgnanoPlatformShutdown();
#endif
#endif

  return 0;
}

extern "C" int SDL_main(int argv, char** args) {
  return main(argv, args);
}
