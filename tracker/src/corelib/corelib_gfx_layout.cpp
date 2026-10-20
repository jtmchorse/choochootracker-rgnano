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

static int presentUnsquashedCols = 0;
void gfxSetPresentUnsquashedCols(int cols) { presentUnsquashedCols = cols; }
int gfxGetPresentUnsquashedCols(void) { return presentUnsquashedCols; }
