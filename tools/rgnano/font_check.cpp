// Legibility harness for the 8x12 font on the RG Nano.
//
// Renders text with font8x12 on a 320x240 logical canvas (40x20 grid), applies
// the same 0.75x horizontal area filter the platform layer uses to reach
// 240x240, and writes PNGs: raw 320x240, squashed 240x240, and a 3x
// nearest-neighbour enlargement of the squashed one. Also prints the closest
// glyph pairs after the squash.
//
// Build/run: make -C tools/rgnano -f font_check.mk   (or see the g++ line below)
//   g++ -O2 -std=c++17 -I../../font-generator -o /tmp/font_check font_check.cpp \
//       ../../tracker/platforms/shared/font8x12.cpp && /tmp/font_check OUT_DIR
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

extern "C" const uint8_t font8x12[];

static const int CW = 8, CH = 12, COLS = 40, ROWS = 20;
static const int W = COLS * CW, H = ROWS * CH;      // 320x240
static const int SW = 240;                          // squashed width

struct Img { int w, h; std::vector<uint8_t> px; Img(int w_, int h_) : w(w_), h(h_), px(w_ * h_ * 3, 0) {} };

static void putc8(Img& im, int col, int row, char ch, uint32_t fg, uint32_t bg) {
  if (col < 0 || col >= im.w / CW) return;
  if (ch < 32 || ch > 126) ch = (char)63;
  const uint8_t* g = font8x12 + (ch - 32) * CH;
  for (int y = 0; y < CH; y++)
    for (int x = 0; x < CW; x++) {
      uint32_t c = (g[y] & (0x80 >> x)) ? fg : bg;
      uint8_t* p = &im.px[((row * CH + y) * im.w + col * CW + x) * 3];
      p[0] = c >> 16; p[1] = c >> 8; p[2] = c;
    }
}
static void fill(Img& im, uint32_t c) {
  for (size_t i = 0; i < im.px.size(); i += 3) { im.px[i] = c >> 16; im.px[i+1] = c >> 8; im.px[i+2] = c; }
}
static void text(Img& im, int col, int row, const std::string& s, uint32_t fg = 0xE0E0E0, uint32_t bg = 0x101018) {
  for (size_t i = 0; i < s.size(); i++) putc8(im, col + (int)i, row, s[i], fg, bg);
}

// Area (box) filter 320 -> 240 horizontally, 1:1 vertically.
static Img squash(const Img& in) {
  Img out(SW, in.h);
  const double r = (double)in.w / SW;               // 4/3 source px per dest px
  for (int y = 0; y < in.h; y++)
    for (int x = 0; x < SW; x++) {
      double a = x * r, b = (x + 1) * r, acc[3] = {0, 0, 0};
      for (int sx = (int)floor(a); sx < (int)ceil(b) && sx < in.w; sx++) {
        double cov = std::min<double>(b, sx + 1) - std::max<double>(a, sx);
        for (int c = 0; c < 3; c++) acc[c] += cov * in.px[(y * in.w + sx) * 3 + c];
      }
      for (int c = 0; c < 3; c++) out.px[(y * SW + x) * 3 + c] = (uint8_t)lround(acc[c] / r);
    }
  return out;
}
static Img zoom(const Img& in, int k) {
  Img out(in.w * k, in.h * k);
  for (int y = 0; y < out.h; y++)
    for (int x = 0; x < out.w; x++)
      memcpy(&out.px[(y * out.w + x) * 3], &in.px[((y / k) * in.w + x / k) * 3], 3);
  return out;
}
static void save(const Img& im, const std::string& path) {
  stbi_write_png(path.c_str(), im.w, im.h, 3, im.px.data(), im.w * 3);
}
static void emit(const Img& raw, const std::string& dir, const std::string& name) {
  Img sq = squash(raw);
  save(raw, dir + "/" + name + "-320x240.png");
  save(sq, dir + "/" + name + "-240x240.png");
  save(zoom(sq, 3), dir + "/" + name + "-240x240-x3.png");
}

// Squashed 6x12 luminance of one glyph (cell squashes to exactly 6 px).
static std::vector<double> glyphSq(char ch) {
  Img im(CW, CH);
  fill(im, 0);
  const uint8_t* g = font8x12 + (ch - 32) * CH;
  for (int y = 0; y < CH; y++)
    for (int x = 0; x < CW; x++)
      if (g[y] & (0x80 >> x)) for (int c = 0; c < 3; c++) im.px[(y * CW + x) * 3 + c] = 255;
  std::vector<double> v;
  for (int y = 0; y < CH; y++)
    for (int x = 0; x < 6; x++) {
      double a = x * 4.0 / 3, b = (x + 1) * 4.0 / 3, acc = 0;
      for (int sx = (int)floor(a); sx < (int)ceil(b) && sx < CW; sx++)
        acc += (std::min<double>(b, sx + 1) - std::max<double>(a, sx)) * im.px[(y * CW + sx) * 3];
      v.push_back(acc / (4.0 / 3) / 255.0);
    }
  return v;
}
static void closest(const char* label, const std::string& set, int topn) {
  struct P { double d; char a, b; };
  std::vector<P> ps;
  std::vector<std::vector<double>> gs;
  for (char c : set) gs.push_back(glyphSq(c));
  for (size_t i = 0; i < set.size(); i++)
    for (size_t j = i + 1; j < set.size(); j++) {
      double d = 0;
      for (size_t k = 0; k < gs[i].size(); k++) d += (gs[i][k] - gs[j][k]) * (gs[i][k] - gs[j][k]);
      ps.push_back({sqrt(d), set[i], set[j]});
    }
  std::sort(ps.begin(), ps.end(), [](const P& x, const P& y) { return x.d < y.d; });
  printf("%s closest pairs (L2 on squashed 6x12, 0 = identical):\n", label);
  for (int i = 0; i < topn && i < (int)ps.size(); i++) printf("  '%c' '%c'  %.2f\n", ps[i].a, ps[i].b, ps[i].d);
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : ".";
  const uint32_t HI_FG = 0x101018, HI_BG = 0xE0E0E0;

  { // (a) glyph sheet: all 95 glyphs, 40 per row, plus a hex/note strip
    Img im(W, H); fill(im, 0x101018);
    for (int i = 0; i < 95; i++) text(im, i % 40, 1 + (i / 40) * 2, std::string(1, (char)(32 + i)));
    text(im, 0, 8, "0123456789ABCDEF  C-4 D#5 F#3 A-2 ---");
    text(im, 0, 10, "0O 1lI| 8B 5S 2Z 6G 9g DO CE FE BR");
    text(im, 0, 12, "0123456789ABCDEF", HI_FG, HI_BG);
    text(im, 0, 14, "The quick brown fox jumps over 13 lazy");
    text(im, 0, 15, "dogs. Volume: 7F  Tempo 125  Pan: C0");
    emit(im, dir, "glyph-sheet");
  }
  { // (b) mock tracker rows
    Img im(W, H); fill(im, 0x101018);
    const char* rows[] = {
      "00 C-4 01 7F A10 B20 ---",
      "01 D#5 02 3F --- C0F E04",
      "02 F#3 0A 5C 3A0 --- ---",
      "03 --- -- -- --- --- ---",
      "04 G-2 0B 7F 4C3 D1E F00",
      "05 A#4 0C 20 E7F B08 C0C",
      "I l 1 | O 0 g q 8 B S 5",
      "0 O 8 B D 5 S 6 G 2 Z 1 I",
      "0123456789 ABCDEF abcdef",
    };
    for (int i = 0; i < 9; i++) text(im, 1, 1 + i * 2, rows[i]);
    text(im, 1, 3, "01 D#5 02 3F --- C0F E04", HI_FG, HI_BG);
    emit(im, dir, "mock-rows");
  }
  { // (c) mock full screen (text approximation of a phrase view, not the real renderer)
    Img im(W, H); fill(im, 0x101018);
    text(im, 0, 0, "PHRASE 0A            SONG 02  TEMPO 125", 0x80C0FF);
    text(im, 0, 2, "  N   I  V  FX1  FX2  FX3", 0xFFC060);
    const char* r[16] = {
      "00 C-4 01 7F A10 B20 ---", "01 --- -- -- --- --- ---", "02 D#5 02 3F --- C0F E04", "03 --- -- -- --- --- ---",
      "04 G-4 01 7F A10 --- ---", "05 --- -- -- E04 --- ---", "06 F#3 0A 5C 3A0 --- ---", "07 --- -- -- --- --- ---",
      "08 C-4 01 7F A10 B20 ---", "09 --- -- -- --- --- ---", "0A D#5 02 3F --- C0F E04", "0B --- -- -- --- --- ---",
      "0C OFF -- -- --- --- ---", "0D A#2 0C 20 E7F B08 C0C", "0E --- -- -- --- --- ---", "0F B-3 01 7F --- --- ---"};
    for (int i = 0; i < 16; i++) text(im, 0, 3 + i, r[i], i == 2 ? HI_FG : 0xE0E0E0, i == 2 ? HI_BG : 0x101018);
    for (int i = 0; i < 8; i++) { text(im, 34, 3 + i, std::to_string(i + 1) + " ~C-4", 0x80FF80); }
    text(im, 34, 14, "MSCPIT", 0xFFC060);
    text(im, 34, 15, " ^    ", 0xE0E0E0);
    text(im, 0, 19, "Note: C-4  Inst: 01  Vol: 7F", 0xA0A0A0);
    emit(im, dir, "mock-screen");
  }

  closest("Tracker set 0-9 A-F # -", "0123456789ABCDEF#-", 12);
  closest("Confusables", "0O1lI|8BDS5Z2G6gq9CEFtiL", 14);
  return 0;
}
