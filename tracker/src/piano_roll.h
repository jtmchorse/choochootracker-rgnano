#pragma once
#include <stdint.h>

// Compact 1.5 octave piano strip for the PHRASE screen, top right.
// Cell box: columns 12..29, rows 0..1 (pixels x 96..239, y 0..23 on 8x12 glyphs).
// Keys span PIANO_ROLL_KEYS_PX pixels, right-justified to the 240 px edge.
#define PIANO_ROLL_COL 12
#define PIANO_ROLL_ROW 0
#define PIANO_ROLL_COLS 18
#define PIANO_ROLL_ROWS 2
#define PIANO_ROLL_KEYS 18      // semitones: C .. F of the next octave
#define PIANO_ROLL_WHITES 11
#define PIANO_ROLL_WHITE_W 13
#define PIANO_ROLL_BLACK_W 9
#define PIANO_ROLL_KEYS_PX (PIANO_ROLL_WHITES * PIANO_ROLL_WHITE_W)

// Window start (a C) for a note: its own octave. Always contains the note.
int pianoRollWindowStart(int note);
// Key index 0..17 of note in the window, or -1 (empty, note-off, outside).
int pianoRollKeyIndex(int note, int windowStart);
int pianoRollIsBlack(int keyIndex);
// Key under a pixel local to the cell box (0,0 = box top left), or -1.
int pianoRollKeyAtPixel(int px, int py);
// Absolute pixel rectangle of the keys.
void pianoRollRect(int* x, int* y, int* w, int* h);

// Redraws only when the highlighted note or window changed (or force).
// note < 0 or >= 254 means no highlight; the window then stays where it was.
void pianoRollDraw(int note, int force);
