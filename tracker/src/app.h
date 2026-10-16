#ifndef __APP_H__
#define __APP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"
#include "corelib/corelib_mainloop.h"
#include "corelib/corelib_input.h"

void appSetup(void);
void appCleanup(void);
void appDraw(void);
void appSetStickLiveMode(StickLiveMode mode);
void appOnEvent(MainLoopEventData eventData);
// Drop held-button state after changing the input mapping. A key-up event may
// otherwise resolve through the new mapping instead of the old one.
void appResetInputState(void);
// The physical input currently being dispatched to the active screen.
InputCode appCurrentInput(void);

// Raw input callback for key mapping screen
extern void (*inputRawCallback)(InputCode input, int isDown);
// True if the app wants this keyboard key: the key mapping screen is capturing, or the key is mapped.
int appInputWantsKey(InputCode input);
// Nonzero while the sequencer is playing (UI frame cap, RG Nano audio stats).
int appIsPlaying(void);


#ifdef __cplusplus
}
#endif

#endif
