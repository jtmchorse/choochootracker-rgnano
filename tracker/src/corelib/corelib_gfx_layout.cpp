#include "corelib_gfx.h"

static int contentRowOffset = 0;
void gfxSetContentRowOffset(int rows) { contentRowOffset = rows; }
int gfxGetContentRowOffset(void) { return contentRowOffset; }

static int pixelOffsetX = 0;
void gfxSetPixelOffsetX(int px) { pixelOffsetX = px; }
int gfxGetPixelOffsetX(void) { return pixelOffsetX; }

static int rectExtraWidth = 0;
void gfxSetRectExtraWidth(int px) { rectExtraWidth = px; }
int gfxGetRectExtraWidth(void) { return rectExtraWidth; }

#ifdef RGNANO_BUILD
// RG Nano: always 1:1 (30 columns); the squashed canvas can never be selected.
static int presentUnsquashedCols = 30;
void gfxSetPresentUnsquashedCols(int cols) { (void)cols; presentUnsquashedCols = 30; }
#else
static int presentUnsquashedCols = 0;
void gfxSetPresentUnsquashedCols(int cols) { presentUnsquashedCols = cols; }
#endif
int gfxGetPresentUnsquashedCols(void) { return presentUnsquashedCols; }
