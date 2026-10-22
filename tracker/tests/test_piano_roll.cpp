#include "doctest.h"
#include "piano_roll.h"
#include "screens/screen_layout.h"

TEST_CASE("piano roll maps notes to keys including black keys") {
  int s = pianoRollWindowStart(60);  // C-5 region
  CHECK(s == 60);
  CHECK(pianoRollKeyIndex(60, s) == 0);
  CHECK(!pianoRollIsBlack(0));
  CHECK(pianoRollKeyIndex(61, s) == 1);
  CHECK(pianoRollIsBlack(1));
  CHECK(pianoRollIsBlack(3));
  CHECK(!pianoRollIsBlack(4));
  CHECK(pianoRollIsBlack(6));
  CHECK(pianoRollIsBlack(8));
  CHECK(pianoRollIsBlack(10));
  CHECK(!pianoRollIsBlack(11));
  CHECK(pianoRollKeyIndex(-1, s) == -1);
  CHECK(pianoRollKeyIndex(254, s) == -1);
  CHECK(pianoRollKeyIndex(255, s) == -1);
}

TEST_CASE("piano roll window shifts by whole octaves") {
  int s = pianoRollWindowStart(60);
  CHECK(pianoRollKeyIndex(71, s) == 11);   // B: last key
  CHECK(pianoRollKeyIndex(72, s) == -1);   // next C: outside
  CHECK(PIANO_ROLL_KEYS == 12);
  CHECK(PIANO_ROLL_WHITES == 7);
  int s2 = pianoRollWindowStart(78);
  CHECK(s2 == 72);
  CHECK(pianoRollKeyIndex(78, s2) == 6);
  CHECK(pianoRollKeyIndex(59, s) == -1);
  CHECK(pianoRollWindowStart(59) == 48);
  for (int n = 0; n < 254; ++n)
    CHECK(pianoRollKeyIndex(n, pianoRollWindowStart(n)) >= 0);
}

TEST_CASE("piano roll pixels hit every key and stay in the box") {
  bool seen[PIANO_ROLL_KEYS] = {};
  for (int y = 0; y < PIANO_ROLL_ROWS * 12; ++y)
    for (int x = 0; x < PIANO_ROLL_COLS * 8; ++x) {
      int k = pianoRollKeyAtPixel(x, y);
      if (k >= 0) { CHECK(k < PIANO_ROLL_KEYS); seen[k] = true; }
    }
  for (int k = 0; k < PIANO_ROLL_KEYS; ++k) CHECK(seen[k]);
}

TEST_CASE("piano roll rectangle is right-justified and clear of data and overlay") {
  int x, y, w, h;
  pianoRollRect(&x, &y, &w, &h);
  CHECK(x + w == 240);
  CHECK(y >= 0);
  CHECK(w == 91);
  CHECK(h == 18);
  CHECK(y + h <= 24);                    // rows 0-1 only
  CHECK(PIANO_ROLL_COL * 8 <= x);
  CHECK(PIANO_ROLL_COL * 8 + PIANO_ROLL_COLS * 8 == 240);
  CHECK(PIANO_ROLL_COL >= 11);           // title "PHRASE 000*" ends at column 10
  CHECK(y + h <= 2 * 12);                // above column header row 2 and data rows 3+
  // Shift map overlay box (cells) does not intersect the roll's cell box.
  CHECK(PIANO_ROLL_ROW + PIANO_ROLL_ROWS <= SHIFT_MAP_Y);
}
