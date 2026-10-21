#include "piano_roll.h"
#include "common.h"
#include "corelib_gfx.h"

static const int whiteSemi[PIANO_ROLL_WHITES] = {0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17};
static const int blackSemi[7] = {1, 3, 6, 8, 10, 13, 15};

static int keysLeft(void) { return PIANO_ROLL_COLS * 8 - PIANO_ROLL_KEYS_PX; }
static const int keyTop = 1, keyH = 22, blackH = 13;

int pianoRollWindowStart(int note) { return note < 0 ? 0 : (note / 12) * 12; }

int pianoRollKeyIndex(int note, int windowStart) {
  if (note < 0 || note >= 254) return -1;
  int i = note - windowStart;
  return (i >= 0 && i < PIANO_ROLL_KEYS) ? i : -1;
}

int pianoRollIsBlack(int k) {
  for (int i = 0; i < 7; ++i) if (blackSemi[i] == k) return 1;
  return 0;
}

int pianoRollKeyAtPixel(int px, int py) {
  px -= keysLeft(); py -= keyTop;
  if (px < 0 || py < 0 || px >= PIANO_ROLL_KEYS_PX || py >= keyH) return -1;
  if (py < blackH) {
    // Black key straddles the boundary after white key w.
    for (int i = 0; i < 7; ++i) {
      int w = 0;
      while (whiteSemi[w] + 1 != blackSemi[i]) ++w;
      int cx = (w + 1) * PIANO_ROLL_WHITE_W;
      if (px >= cx - PIANO_ROLL_BLACK_W / 2 && px < cx - PIANO_ROLL_BLACK_W / 2 + PIANO_ROLL_BLACK_W) return blackSemi[i];
    }
  }
  int w = px / PIANO_ROLL_WHITE_W;
  // 1 px gap on the right of each white key
  if (px % PIANO_ROLL_WHITE_W == PIANO_ROLL_WHITE_W - 1) return -1;
  return whiteSemi[w];
}

void pianoRollRect(int* x, int* y, int* w, int* h) {
  if (x) *x = PIANO_ROLL_COL * 8 + keysLeft();
  if (y) *y = PIANO_ROLL_ROW * 12 + keyTop;
  if (w) *w = PIANO_ROLL_KEYS_PX;
  if (h) *h = keyH;
}

static Bitmap* bmp;
static int lastKey = -2, lastStart = -1, curStart = 48;

void pianoRollDraw(int note, int force) {
  int key = -1;
  if (note >= 0 && note < 254) { curStart = pianoRollWindowStart(note); key = note - curStart; }
  if (!force && key == lastKey && curStart == lastStart) return;
  lastKey = key; lastStart = curStart;
  if (bmp && (bmp->widthChars != PIANO_ROLL_COLS || bmp->heightChars != PIANO_ROLL_ROWS)) { gfxBitmapFree(bmp); bmp = nullptr; }
  if (!bmp) bmp = gfxBitmapCreate(PIANO_ROLL_COLS, PIANO_ROLL_ROWS);
  if (!bmp) return;
  const auto cs = appSettings.colorScheme;
  gfxSetBgColor(cs.background);
  gfxClearRect(PIANO_ROLL_COL, PIANO_ROLL_ROW, PIANO_ROLL_COLS, PIANO_ROLL_ROWS);
  const int W = bmp->widthPixels, H = bmp->heightPixels;
  for (int layer = 0; layer < 3; ++layer) {
    gfxBitmapClear(bmp);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        int k = pianoRollKeyAtPixel(x, y);
        if (k < 0) continue;
        bool black = pianoRollIsBlack(k);
        if (layer == 0 && !black) bmp->data[y * W + x] = 70;
        else if (layer == 1 && black) bmp->data[y * W + x] = 255;
        else if (layer == 2 && k == key) bmp->data[y * W + x] = 255;
      }
    gfxSetFgColor(layer == 0 ? cs.textDefault : layer == 1 ? cs.textInfo : cs.textValue);
    gfxDrawBitmap(bmp, PIANO_ROLL_COL, PIANO_ROLL_ROW);
  }
}
