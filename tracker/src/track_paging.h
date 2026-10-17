#ifndef TRACK_PAGING_H
#define TRACK_PAGING_H

// "4x2" track view: the Song screen shows 4 tracks at a time on two pages
// (tracks 1-4, tracks 5-8). The cursor column is always the absolute track
// index; the page is derived from it, so moving the cursor past the edge of a
// page flips the page with no extra state. Pure functions, no screen deps.

#define TRACK_VIEW_ALL 0     // all tracks on one page (upstream layout)
#define TRACK_VIEW_4X2 1     // 4 tracks per page
#define TRACK_PAGE_SIZE 4

static inline int trackViewPaged(int trackView, int tracksCount) {
  return trackView == TRACK_VIEW_4X2 && tracksCount > TRACK_PAGE_SIZE;
}

static inline int trackPageOf(int track) { return track / TRACK_PAGE_SIZE; }

static inline int trackPageCount(int tracksCount) {
  return (tracksCount + TRACK_PAGE_SIZE - 1) / TRACK_PAGE_SIZE;
}

// First and last track (inclusive) shown on a page.
static inline int trackPageFirst(int page) { return page * TRACK_PAGE_SIZE; }
static inline int trackPageLast(int page, int tracksCount) {
  int last = page * TRACK_PAGE_SIZE + TRACK_PAGE_SIZE - 1;
  return last < tracksCount - 1 ? last : tracksCount - 1;
}

// Screen slot (0..3) of a track on its page, or -1 if the track is not on
// `page`. Unpaged view: slot is the track itself.
static inline int trackSlot(int track, int page, int paged) {
  if (!paged) return track;
  return trackPageOf(track) == page ? track % TRACK_PAGE_SIZE : -1;
}

// Inverse: track shown in `slot` of `page`, or -1 if past the last track.
static inline int slotTrack(int slot, int page, int tracksCount) {
  int track = page * TRACK_PAGE_SIZE + slot;
  return (slot >= 0 && slot < TRACK_PAGE_SIZE && track < tracksCount) ? track : -1;
}

// Cursor column after an explicit page move (dir -1 = previous page, +1 =
// next). Keeps the same slot, clamped to the last track; unchanged at the ends.
static inline int trackPageMove(int cursorCol, int dir, int tracksCount) {
  int page = trackPageOf(cursorCol) + dir;
  if (page < 0 || page >= trackPageCount(tracksCount)) return cursorCol;
  int col = page * TRACK_PAGE_SIZE + cursorCol % TRACK_PAGE_SIZE;
  return col < tracksCount ? col : tracksCount - 1;
}

#endif
