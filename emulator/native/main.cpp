// AALU native emulator — milestone 1.
//
// Single-file C++ server that:
//   - Serves the static web UI from /web.
//   - Speaks the AALU emulator WebSocket protocol (1bpp framebuffer + JSON control).
//   - Renders a minimal interactive shell (file browser + log) using an embedded
//     8x16 ASCII font.
//
// This is intentionally NOT linked against lib/ or src/. The HAL is too entangled
// with Arduino/FreeRTOS to compile cleanly natively in one shot; integrating real
// lib/EpdFont rasterization + the activity loop is milestone 2.

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <netinet/in.h>
#include <optional>
#include <openssl/sha.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// ──────────────────────────────────────────────────────────────────────────────
// Config
// ──────────────────────────────────────────────────────────────────────────────

namespace cfg {
// Physical framebuffer dimensions — always landscape, matches the e-ink panel.
constexpr int      SCREEN_W       = 800;
constexpr int      SCREEN_H       = 480;
constexpr size_t   FB_BYTES       = (SCREEN_W * SCREEN_H) / 8;
constexpr int      DEFAULT_PORT   = 8080;
constexpr int      MAX_WS_PAYLOAD = 1 << 20;  // 1 MiB — plenty for our control frames

const char* env_or(const char* name, const char* fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : fallback;
}
}  // namespace cfg

enum class Orientation { Landscape, Portrait };

// ──────────────────────────────────────────────────────────────────────────────
// Embedded 8x16 ASCII font (IBM VGA classic, public domain).
// One row per byte, 16 rows per glyph, 8 columns. Bit 7 = leftmost pixel.
// Covers 0x20..0x7E.
// ──────────────────────────────────────────────────────────────────────────────

constexpr int FONT_W = 8;
constexpr int FONT_H = 16;

static const uint8_t FONT8x16[96][16] = {
  // 0x20 ' '
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
  // 0x21 '!'
  {0,0,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
  // 0x22 '"'
  {0,0x66,0x66,0x66,0x24,0,0,0,0,0,0,0,0,0,0,0},
  // 0x23 '#'
  {0,0,0,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0,0,0,0},
  // 0x24 '$'
  {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x06,0x86,0xC6,0x7C,0x18,0x18,0,0},
  // 0x25 '%'
  {0,0,0,0,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0,0,0,0},
  // 0x26 '&'
  {0,0,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0,0,0,0},
  // 0x27 '\''
  {0,0x30,0x30,0x30,0x60,0,0,0,0,0,0,0,0,0,0,0},
  // 0x28 '('
  {0,0,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0,0,0,0},
  // 0x29 ')'
  {0,0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0,0,0,0},
  // 0x2A '*'
  {0,0,0,0,0,0x66,0x3C,0xFF,0x3C,0x66,0,0,0,0,0,0},
  // 0x2B '+'
  {0,0,0,0,0,0x18,0x18,0x7E,0x18,0x18,0,0,0,0,0,0},
  // 0x2C ','
  {0,0,0,0,0,0,0,0,0,0,0x18,0x18,0x18,0x30,0,0},
  // 0x2D '-'
  {0,0,0,0,0,0,0,0x7E,0,0,0,0,0,0,0,0},
  // 0x2E '.'
  {0,0,0,0,0,0,0,0,0,0,0x18,0x18,0,0,0,0},
  // 0x2F '/'
  {0,0,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0,0,0,0,0,0},
  // 0x30 '0'
  {0,0,0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
  // 0x31 '1'
  {0,0,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0},
  // 0x32 '2'
  {0,0,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0,0,0,0},
  // 0x33 '3'
  {0,0,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0,0,0,0},
  // 0x34 '4'
  {0,0,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0,0,0,0},
  // 0x35 '5'
  {0,0,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0,0,0,0},
  // 0x36 '6'
  {0,0,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
  // 0x37 '7'
  {0,0,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0,0,0,0},
  // 0x38 '8'
  {0,0,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
  // 0x39 '9'
  {0,0,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0,0,0,0},
  // 0x3A ':'
  {0,0,0,0,0x18,0x18,0,0,0,0x18,0x18,0,0,0,0,0},
  // 0x3B ';'
  {0,0,0,0,0x18,0x18,0,0,0,0x18,0x18,0x30,0,0,0,0},
  // 0x3C '<'
  {0,0,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0,0,0,0,0},
  // 0x3D '='
  {0,0,0,0,0,0x7E,0,0,0x7E,0,0,0,0,0,0,0},
  // 0x3E '>'
  {0,0,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0,0,0,0,0},
  // 0x3F '?'
  {0,0,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
  // 0x40 '@'
  {0,0,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0xC0,0x7C,0,0,0,0},
  // 0x41 'A'
  {0,0,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
  // 0x42 'B'
  {0,0,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0,0,0,0},
  // 0x43 'C'
  {0,0,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0,0,0,0},
  // 0x44 'D'
  {0,0,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0,0,0,0},
  // 0x45 'E'
  {0,0,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0,0,0,0},
  // 0x46 'F'
  {0,0,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0,0,0,0},
  // 0x47 'G'
  {0,0,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0,0,0,0},
  // 0x48 'H'
  {0,0,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
  // 0x49 'I'
  {0,0,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
  // 0x4A 'J'
  {0,0,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0,0,0,0},
  // 0x4B 'K'
  {0,0,0xE6,0x66,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0,0,0,0},
  // 0x4C 'L'
  {0,0,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0,0,0,0},
  // 0x4D 'M'
  {0,0,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
  // 0x4E 'N'
  {0,0,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0,0,0,0},
  // 0x4F 'O'
  {0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
  // 0x50 'P'
  {0,0,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0,0,0,0},
  // 0x51 'Q'
  {0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0,0},
  // 0x52 'R'
  {0,0,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0,0,0,0},
  // 0x53 'S'
  {0,0,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0,0,0,0},
  // 0x54 'T'
  {0,0,0x7E,0x7E,0x5A,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
  // 0x55 'U'
  {0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
  // 0x56 'V'
  {0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0,0,0,0},
  // 0x57 'W'
  {0,0,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0,0,0,0},
  // 0x58 'X'
  {0,0,0xC6,0xC6,0x6C,0x7C,0x38,0x38,0x7C,0x6C,0xC6,0xC6,0,0,0,0},
  // 0x59 'Y'
  {0,0,0x66,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
  // 0x5A 'Z'
  {0,0,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0,0,0,0},
  // 0x5B '['
  {0,0,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0},
  // 0x5C '\'
  {0,0,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0,0,0,0,0},
  // 0x5D ']'
  {0,0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0},
  // 0x5E '^'
  {0x10,0x38,0x6C,0xC6,0,0,0,0,0,0,0,0,0,0,0,0},
  // 0x5F '_'
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0xFF,0},
  // 0x60 '`'
  {0x30,0x30,0x18,0,0,0,0,0,0,0,0,0,0,0,0,0},
  // 0x61 'a'
  {0,0,0,0,0,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0,0,0,0},
  // 0x62 'b'
  {0,0,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0,0,0,0},
  // 0x63 'c'
  {0,0,0,0,0,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0,0,0,0},
  // 0x64 'd'
  {0,0,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0},
  // 0x65 'e'
  {0,0,0,0,0,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0,0,0,0},
  // 0x66 'f'
  {0,0,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0,0,0,0},
  // 0x67 'g'
  {0,0,0,0,0,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0},
  // 0x68 'h'
  {0,0,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0,0,0,0},
  // 0x69 'i'
  {0,0,0x18,0x18,0,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
  // 0x6A 'j'
  {0,0,0x06,0x06,0,0x0E,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0,0},
  // 0x6B 'k'
  {0,0,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0,0,0,0},
  // 0x6C 'l'
  {0,0,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0},
  // 0x6D 'm'
  {0,0,0,0,0,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0,0,0,0},
  // 0x6E 'n'
  {0,0,0,0,0,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0,0,0,0},
  // 0x6F 'o'
  {0,0,0,0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0},
  // 0x70 'p'
  {0,0,0,0,0,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0},
  // 0x71 'q'
  {0,0,0,0,0,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0},
  // 0x72 'r'
  {0,0,0,0,0,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0,0,0,0},
  // 0x73 's'
  {0,0,0,0,0,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0,0,0,0},
  // 0x74 't'
  {0,0,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0,0,0,0},
  // 0x75 'u'
  {0,0,0,0,0,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0},
  // 0x76 'v'
  {0,0,0,0,0,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0,0,0,0},
  // 0x77 'w'
  {0,0,0,0,0,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0,0,0,0},
  // 0x78 'x'
  {0,0,0,0,0,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0,0,0,0},
  // 0x79 'y'
  {0,0,0,0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0},
  // 0x7A 'z'
  {0,0,0,0,0,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0,0,0,0},
  // 0x7B '{'
  {0,0,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0,0,0,0},
  // 0x7C '|'
  {0,0,0x18,0x18,0x18,0x18,0,0x18,0x18,0x18,0x18,0x18,0,0,0,0},
  // 0x7D '}'
  {0,0,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0,0,0,0},
  // 0x7E '~'
  {0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0,0},
  // 0x7F (placeholder)
  {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

// ──────────────────────────────────────────────────────────────────────────────
// Framebuffer — 1bpp packed, same layout as the device.
// ──────────────────────────────────────────────────────────────────────────────

struct Framebuffer {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(cfg::FB_BYTES, 0xFF);  // white background
  Orientation orientation = Orientation::Portrait;

  // Logical dimensions follow the orientation. In portrait the user sees a
  // 480-wide × 800-tall canvas (the CSS rotate(90deg) on the client unrotates it).
  int logicalW() const { return orientation == Orientation::Portrait ? cfg::SCREEN_H : cfg::SCREEN_W; }
  int logicalH() const { return orientation == Orientation::Portrait ? cfg::SCREEN_W : cfg::SCREEN_H; }

  void clear(bool white = true) { std::fill(bytes.begin(), bytes.end(), white ? 0xFF : 0x00); }

  // setPixel takes LOGICAL coordinates. In portrait mode we translate to
  // physical coords so that the CSS rotate(90deg) on the canvas displays them
  // upright. CW rotation: logical (lx, ly) → physical (px=ly, py=SCREEN_H-1-lx).
  void setPixel(int lx, int ly, bool white) {
    int px, py;
    if (orientation == Orientation::Portrait) {
      if (lx < 0 || lx >= cfg::SCREEN_H || ly < 0 || ly >= cfg::SCREEN_W) return;
      px = ly;
      py = cfg::SCREEN_H - 1 - lx;
    } else {
      if (lx < 0 || lx >= cfg::SCREEN_W || ly < 0 || ly >= cfg::SCREEN_H) return;
      px = lx;
      py = ly;
    }
    const size_t idx = (py * cfg::SCREEN_W + px) / 8;
    const uint8_t mask = 0x80 >> (px % 8);
    if (white) bytes[idx] |= mask;
    else       bytes[idx] &= ~mask;
  }

  void fillRect(int x, int y, int w, int h, bool white) {
    for (int dy = 0; dy < h; ++dy)
      for (int dx = 0; dx < w; ++dx)
        setPixel(x + dx, y + dy, white);
  }

  void drawHLine(int x, int y, int w, bool white) { fillRect(x, y, w, 1, white); }
  void drawVLine(int x, int y, int h, bool white) { fillRect(x, y, 1, h, white); }

  void drawRect(int x, int y, int w, int h, bool white) {
    drawHLine(x, y, w, white);
    drawHLine(x, y + h - 1, w, white);
    drawVLine(x, y, h, white);
    drawVLine(x + w - 1, y, h, white);
  }

  void drawChar(int x, int y, char c, bool black = true) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t* glyph = FONT8x16[c - 0x20];
    for (int row = 0; row < FONT_H; ++row) {
      const uint8_t bits = glyph[row];
      for (int col = 0; col < FONT_W; ++col) {
        if (bits & (0x80 >> col)) setPixel(x + col, y + row, !black);
      }
    }
  }

  void drawText(int x, int y, std::string_view s, bool black = true) {
    int cx = x;
    for (char c : s) {
      if (c == '\n') { y += FONT_H; cx = x; continue; }
      drawChar(cx, y, c, black);
      cx += FONT_W;
    }
  }

  // Truncate with ellipsis if string doesn't fit in maxPx pixels.
  void drawTextClipped(int x, int y, std::string_view s, int maxPx, bool black = true) {
    const int maxChars = maxPx / FONT_W;
    if ((int)s.size() <= maxChars) {
      drawText(x, y, s, black);
    } else if (maxChars > 3) {
      std::string clipped(s.substr(0, maxChars - 3));
      clipped += "...";
      drawText(x, y, clipped, black);
    } else {
      drawText(x, y, s.substr(0, maxChars), black);
    }
  }
};

// ──────────────────────────────────────────────────────────────────────────────
// UI state — file browser + log pane.
// ──────────────────────────────────────────────────────────────────────────────

struct DirEntry {
  std::string name;
  std::string path;
  bool isDir;
  uint64_t size;
};

struct UiState {
  std::string sdRoot{"/sdcard"};
  std::string cwd{"/sdcard"};
  std::vector<DirEntry> entries;
  int selected = 0;
  int scrollTop = 0;
  std::vector<std::string> log;

  // Recomputed when the framebuffer's orientation changes — see Server::handleClientMessage.
  // Landscape: ~22 rows fit between title bar (y=36) and divider (y=390) at 16px row height.
  // Portrait: ~44 rows fit in the taller logical canvas.
  int visibleRows = 22;

  void pushLog(const std::string& msg) {
    log.push_back(msg);
    if (log.size() > 8) log.erase(log.begin(), log.begin() + (log.size() - 8));
  }

  void rescan() {
    entries.clear();
    if (cwd != sdRoot) {
      entries.push_back({"..", fs::path(cwd).parent_path().string(), true, 0});
    }
    try {
      std::vector<DirEntry> dirs, files;
      for (const auto& de : fs::directory_iterator(cwd)) {
        const auto& p = de.path();
        const std::string name = p.filename().string();
        if (name.empty() || name[0] == '.') continue;  // skip dotfiles incl .crosspoint
        DirEntry e{name, p.string(), de.is_directory(), 0};
        if (!e.isDir) {
          std::error_code ec;
          e.size = fs::file_size(p, ec);
        }
        (e.isDir ? dirs : files).push_back(std::move(e));
      }
      auto sortByName = [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; };
      std::sort(dirs.begin(), dirs.end(), sortByName);
      std::sort(files.begin(), files.end(), sortByName);
      entries.insert(entries.end(), dirs.begin(), dirs.end());
      entries.insert(entries.end(), files.begin(), files.end());
    } catch (const std::exception& e) {
      pushLog(std::string("scan err: ") + e.what());
    }
    selected = std::min(selected, (int)entries.size() - 1);
    if (selected < 0) selected = 0;
    if (selected < scrollTop) scrollTop = selected;
    if (selected >= scrollTop + visibleRows) scrollTop = selected - visibleRows + 1;
    pushLog(std::string("rescan ") + cwd + " -> " + std::to_string(entries.size()) + " entries");
  }

  void moveSelection(int delta) {
    if (entries.empty()) return;
    selected = std::clamp(selected + delta, 0, (int)entries.size() - 1);
    if (selected < scrollTop) scrollTop = selected;
    if (selected >= scrollTop + visibleRows) scrollTop = selected - visibleRows + 1;
  }

  void activate() {
    if (entries.empty()) return;
    const DirEntry& e = entries[selected];
    if (e.isDir) {
      cwd = e.path;
      selected = 0;
      scrollTop = 0;
      rescan();
    } else {
      pushLog("open: " + e.name + " (stub — reader not wired yet)");
    }
  }

  void goBack() {
    if (cwd == sdRoot) {
      pushLog("at sd root");
      return;
    }
    cwd = fs::path(cwd).parent_path().string();
    selected = 0;
    scrollTop = 0;
    rescan();
  }
};

static std::string humanSize(uint64_t n) {
  char buf[32];
  if (n < 1024)        std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)n);
  else if (n < 1ULL<<20) std::snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
  else if (n < 1ULL<<30) std::snprintf(buf, sizeof(buf), "%.1f MB", n / (1024.0*1024));
  else                   std::snprintf(buf, sizeof(buf), "%.1f GB", n / (1024.0*1024*1024));
  return buf;
}

// Stroke a string in white-on-black (inverted) at (x, y).
static void drawTextWhite(Framebuffer& fb, int x, int y, std::string_view s, int maxRight) {
  int cx = x;
  for (char c : s) {
    if (cx + FONT_W > maxRight) break;
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t* glyph = FONT8x16[c - 0x20];
    for (int row = 0; row < FONT_H; ++row) {
      const uint8_t bits = glyph[row];
      for (int col = 0; col < FONT_W; ++col) {
        if (bits & (0x80 >> col)) fb.setPixel(cx + col, y + row, true);
      }
    }
    cx += FONT_W;
  }
}

static void renderUi(Framebuffer& fb, const UiState& ui) {
  fb.clear(true);
  const int W = fb.logicalW();
  const int H = fb.logicalH();

  // Title bar
  fb.fillRect(0, 0, W, 28, false);
  drawTextWhite(fb, 12, 6, "AALU emulator", W);

  // CWD label (top right)
  {
    std::string cwdShort = ui.cwd;
    if (cwdShort.rfind(ui.sdRoot, 0) == 0) cwdShort = "sd:" + cwdShort.substr(ui.sdRoot.size());
    if (cwdShort.empty()) cwdShort = "sd:/";
    int cx = W - 12 - (int)cwdShort.size() * FONT_W;
    if (cx < 200) cx = 200;
    drawTextWhite(fb, cx, 6, cwdShort, W - 12);
  }

  // Layout: title (0..28), list (36..dividerY), log/footer at the bottom.
  const int footerH = 24;
  const int logH    = 5 * 16 + 6;  // 5 lines + small gap
  const int dividerY = H - footerH - logH - 4;

  const int listX = 12;
  const int listY = 36;
  const int rowH  = 16;
  const int listH = dividerY - listY;
  // Update the cached row capacity used elsewhere — derived from layout.
  // (UiState::visibleRows is what scrollbar/page-nav refer to; kept in sync by Server.)

  if (ui.entries.empty()) {
    fb.drawText(listX, listY + 8, "(empty directory — drop .epub into emulator/sdcard/)", true);
  } else {
    const int rows = std::min({ui.visibleRows, listH / rowH, (int)ui.entries.size() - ui.scrollTop});
    for (int i = 0; i < rows; ++i) {
      const int idx = ui.scrollTop + i;
      const DirEntry& e = ui.entries[idx];
      const int y = listY + i * rowH;
      const bool selected = (idx == ui.selected);

      if (selected) {
        fb.fillRect(listX - 4, y - 2, W - 24, rowH, false);
      }

      const std::string icon = e.isDir ? "[D]" : "   ";
      const std::string& name = e.name;
      const std::string sz = e.isDir ? "" : humanSize(e.size);

      if (selected) {
        drawTextWhite(fb, listX, y, icon, W);
        drawTextWhite(fb, listX + 4 * FONT_W, y, name, W);
        if (!sz.empty()) {
          int szX = W - 24 - (int)sz.size() * FONT_W;
          drawTextWhite(fb, szX, y, sz, W);
        }
      } else {
        fb.drawText(listX, y, icon, true);
        fb.drawText(listX + 4 * FONT_W, y, name, true);
        if (!sz.empty()) {
          int szX = W - 24 - (int)sz.size() * FONT_W;
          fb.drawText(szX, y, sz, true);
        }
      }
    }

    // Scrollbar
    if ((int)ui.entries.size() > ui.visibleRows) {
      const int barX = W - 16;
      const int trackY = listY;
      const int trackH = ui.visibleRows * rowH;
      fb.drawVLine(barX, trackY, trackH, false);
      const int knobH = std::max(8, trackH * ui.visibleRows / (int)ui.entries.size());
      const int knobY = trackY + (trackH - knobH) * ui.scrollTop / std::max(1, (int)ui.entries.size() - ui.visibleRows);
      fb.fillRect(barX - 1, knobY, 3, knobH, false);
    }
  }

  // Divider
  fb.drawHLine(0, dividerY, W, false);

  // Log pane (last 5 lines)
  const int logY = dividerY + 6;
  fb.drawText(12, logY, "Log:", true);
  const int logRows = std::min<size_t>(5, ui.log.size());
  for (size_t i = 0; i < (size_t)logRows; ++i) {
    const auto& line = ui.log[ui.log.size() - logRows + i];
    fb.drawTextClipped(60, logY + (int)i * 16, line, W - 72, true);
  }

  // Footer hint
  fb.fillRect(0, H - footerH, W, footerH, false);
  drawTextWhite(fb, 12, H - footerH + 4, "Side UP/DOWN: scroll   F2: open   F1: parent   F3/F4: page", W);
}

// ──────────────────────────────────────────────────────────────────────────────
// Minimal HTTP + WebSocket server
// ──────────────────────────────────────────────────────────────────────────────

static std::string base64Encode(const unsigned char* data, size_t len) {
  static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = data[i] << 16;
    if (i + 1 < len) v |= data[i + 1] << 8;
    if (i + 2 < len) v |= data[i + 2];
    out.push_back(tbl[(v >> 18) & 0x3F]);
    out.push_back(tbl[(v >> 12) & 0x3F]);
    out.push_back(i + 1 < len ? tbl[(v >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < len ? tbl[v & 0x3F] : '=');
  }
  return out;
}

static std::string wsAcceptKey(const std::string& clientKey) {
  static const std::string magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const std::string concat = clientKey + magic;
  unsigned char hash[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(concat.data()), concat.size(), hash);
  return base64Encode(hash, SHA_DIGEST_LENGTH);
}

static bool sendAll(int fd, const void* buf, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (len > 0) {
    ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

static bool recvAll(int fd, void* buf, size_t len) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (len > 0) {
    ssize_t n = ::recv(fd, p, len, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    p += n;
    len -= n;
  }
  return true;
}

// Write a single WebSocket frame (FIN=1, server→client, unmasked).
static bool wsSendFrame(int fd, uint8_t opcode, const void* payload, size_t len) {
  uint8_t header[14];
  size_t headerLen = 0;
  header[0] = 0x80 | (opcode & 0x0F);
  if (len < 126) {
    header[1] = (uint8_t)len;
    headerLen = 2;
  } else if (len < 65536) {
    header[1] = 126;
    header[2] = (uint8_t)((len >> 8) & 0xFF);
    header[3] = (uint8_t)(len & 0xFF);
    headerLen = 4;
  } else {
    header[1] = 127;
    uint64_t big = len;
    for (int i = 0; i < 8; ++i) header[2 + i] = (uint8_t)((big >> (56 - 8 * i)) & 0xFF);
    headerLen = 10;
  }
  if (!sendAll(fd, header, headerLen)) return false;
  if (len > 0 && !sendAll(fd, payload, len)) return false;
  return true;
}

static bool wsSendText(int fd, const std::string& s)       { return wsSendFrame(fd, 0x1, s.data(), s.size()); }
static bool wsSendBinary(int fd, const void* p, size_t n)  { return wsSendFrame(fd, 0x2, p, n); }
static bool wsSendClose(int fd)                            { return wsSendFrame(fd, 0x8, nullptr, 0); }
static bool wsSendPong(int fd, const void* p, size_t n)    { return wsSendFrame(fd, 0xA, p, n); }

// Read a single WebSocket frame. Out: opcode, payload bytes.
// Returns false on disconnect or malformed frame.
static bool wsRecvFrame(int fd, uint8_t& opcode, std::vector<uint8_t>& payload) {
  uint8_t hdr[2];
  if (!recvAll(fd, hdr, 2)) return false;
  const bool fin = (hdr[0] & 0x80) != 0;
  opcode = hdr[0] & 0x0F;
  const bool masked = (hdr[1] & 0x80) != 0;
  uint64_t len = hdr[1] & 0x7F;
  if (len == 126) {
    uint8_t ext[2];
    if (!recvAll(fd, ext, 2)) return false;
    len = ((uint64_t)ext[0] << 8) | ext[1];
  } else if (len == 127) {
    uint8_t ext[8];
    if (!recvAll(fd, ext, 8)) return false;
    len = 0;
    for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
  }
  if (len > (uint64_t)cfg::MAX_WS_PAYLOAD) return false;
  uint8_t mask[4] = {0, 0, 0, 0};
  if (masked) {
    if (!recvAll(fd, mask, 4)) return false;
  }
  payload.resize(len);
  if (len > 0) {
    if (!recvAll(fd, payload.data(), len)) return false;
    if (masked) {
      for (size_t i = 0; i < len; ++i) payload[i] ^= mask[i & 3];
    }
  }
  // Note: ignoring fragmentation. AALU client sends only complete short frames.
  (void)fin;
  return true;
}

// Trim ASCII whitespace
static std::string trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  size_t b = s.find_last_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  return s.substr(a, b - a + 1);
}

static std::string toLower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

struct HttpRequest {
  std::string method;
  std::string target;
  std::string version;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string header(const std::string& name) const {
    const std::string key = toLower(name);
    for (const auto& [k, v] : headers) if (toLower(k) == key) return v;
    return "";
  }
};

static std::optional<HttpRequest> readHttpRequest(int fd) {
  std::string buf;
  buf.reserve(1024);
  char c;
  while (true) {
    ssize_t n = ::recv(fd, &c, 1, 0);
    if (n <= 0) return std::nullopt;
    buf.push_back(c);
    if (buf.size() >= 4 && buf.compare(buf.size() - 4, 4, "\r\n\r\n") == 0) break;
    if (buf.size() > 16384) return std::nullopt;
  }
  HttpRequest req;
  size_t lineStart = 0;
  size_t nl = buf.find("\r\n");
  if (nl == std::string::npos) return std::nullopt;
  {
    std::string line = buf.substr(0, nl);
    std::istringstream iss(line);
    iss >> req.method >> req.target >> req.version;
    if (req.method.empty() || req.target.empty()) return std::nullopt;
  }
  lineStart = nl + 2;
  while (lineStart < buf.size() - 2) {
    size_t end = buf.find("\r\n", lineStart);
    if (end == std::string::npos) break;
    if (end == lineStart) break;
    std::string line = buf.substr(lineStart, end - lineStart);
    size_t colon = line.find(':');
    if (colon != std::string::npos) {
      std::string k = trim(line.substr(0, colon));
      std::string v = trim(line.substr(colon + 1));
      req.headers.emplace_back(k, v);
    }
    lineStart = end + 2;
  }
  return req;
}

static std::string mimeFor(const std::string& path) {
  if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".html") == 0) return "text/html; charset=utf-8";
  if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".css")  == 0) return "text/css; charset=utf-8";
  if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".js")   == 0) return "application/javascript; charset=utf-8";
  if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".svg")  == 0) return "image/svg+xml";
  if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".png")  == 0) return "image/png";
  return "application/octet-stream";
}

static void sendHttpResponse(int fd, int status, const std::string& reason, const std::string& contentType,
                              const std::string& body) {
  std::ostringstream os;
  os << "HTTP/1.1 " << status << " " << reason << "\r\n"
     << "Content-Type: " << contentType << "\r\n"
     << "Content-Length: " << body.size() << "\r\n"
     << "Connection: close\r\n\r\n";
  sendAll(fd, os.str().data(), os.str().size());
  if (!body.empty()) sendAll(fd, body.data(), body.size());
}

static std::string readFileToString(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Path traversal guard: only serve files inside webRoot, no ..
static std::string resolveStatic(const std::string& webRoot, std::string urlPath) {
  if (urlPath == "/") urlPath = "/index.html";
  if (urlPath.find("..") != std::string::npos) return "";
  return webRoot + urlPath;
}

// ──────────────────────────────────────────────────────────────────────────────
// JSON — extremely small encoder/decoder for our flat object protocol.
// ──────────────────────────────────────────────────────────────────────────────

static std::string jsonEscape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if ((unsigned char)c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
          o += buf;
        } else {
          o += c;
        }
    }
  }
  return o;
}

// Pull a flat-object string field. Returns "" if not found. Naive — handles our messages only.
static std::string jsonField(const std::string& s, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t p = s.find(needle);
  if (p == std::string::npos) return "";
  p = s.find(':', p);
  if (p == std::string::npos) return "";
  ++p;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
  if (p >= s.size()) return "";
  if (s[p] == '"') {
    ++p;
    std::string out;
    while (p < s.size() && s[p] != '"') {
      if (s[p] == '\\' && p + 1 < s.size()) {
        char n = s[p + 1];
        if (n == 'n') out += '\n';
        else if (n == 'r') out += '\r';
        else if (n == 't') out += '\t';
        else out += n;
        p += 2;
      } else {
        out += s[p++];
      }
    }
    return out;
  }
  // bare token (number/bool)
  std::string out;
  while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ' ') out += s[p++];
  return out;
}

// ──────────────────────────────────────────────────────────────────────────────
// Server state shared between accept loop, render thread, and client threads.
// ──────────────────────────────────────────────────────────────────────────────

struct Server {
  std::string webRoot;
  std::string sdRoot;
  int port;

  std::mutex stateMu;
  UiState ui;
  Framebuffer fb;
  std::atomic<bool> dirty{true};

  std::mutex clientsMu;
  std::vector<int> clientFds;

  std::atomic<bool> shuttingDown{false};

  void log(const std::string& msg) {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&t));
    std::fprintf(stderr, "[%s] %s\n", ts, msg.c_str());
    std::fflush(stderr);
  }

  void broadcastLog(const std::string& msg) {
    std::string j = "{\"type\":\"log\",\"message\":\"" + jsonEscape(msg) + "\"}";
    std::lock_guard<std::mutex> lk(clientsMu);
    for (int fd : clientFds) wsSendText(fd, j);
  }

  void broadcastFramebuffer() {
    std::vector<uint8_t> snapshot;
    {
      std::lock_guard<std::mutex> lk(stateMu);
      snapshot = fb.bytes;
    }
    std::lock_guard<std::mutex> lk(clientsMu);
    for (int fd : clientFds) wsSendBinary(fd, snapshot.data(), snapshot.size());
  }

  void sendInitialState(int fd) {
    int lw, lh;
    {
      std::lock_guard<std::mutex> lk(stateMu);
      lw = fb.logicalW();
      lh = fb.logicalH();
    }
    std::string cfgMsg = "{\"type\":\"config\",\"width\":" + std::to_string(lw) +
                         ",\"height\":" + std::to_string(lh) + "}";
    wsSendText(fd, cfgMsg);
    wsSendText(fd, "{\"type\":\"log\",\"message\":\"native emulator connected\"}");
    std::vector<uint8_t> snapshot;
    {
      std::lock_guard<std::mutex> lk(stateMu);
      snapshot = fb.bytes;
    }
    wsSendBinary(fd, snapshot.data(), snapshot.size());
  }

  // Called from client threads when a control message comes in.
  void handleClientMessage(const std::string& payload) {
    const std::string type = jsonField(payload, "type");
    if (type == "button") {
      const std::string btn = jsonField(payload, "button");
      const std::string action = jsonField(payload, "action");
      if (action != "press") return;  // act on press only — release is no-op
      std::lock_guard<std::mutex> lk(stateMu);
      // Generic physical buttons (3 side + 4 front). Behaviour is firmware-owned
      // per screen in the real OS — milestone-1 file browser maps as follows:
      //   side  UP / DOWN  → scroll selection
      //   side  POWER       → log only (milestone 2 will deep-sleep)
      //   front 1 (◀ left)  → go up a directory
      //   front 2 (▶ right) → enter / activate
      //   front 3 (◀ left)  → page up
      //   front 4 (▶ right) → page down
      if      (btn == "SIDE_UP")    ui.moveSelection(-1);
      else if (btn == "SIDE_DOWN")  ui.moveSelection(1);
      else if (btn == "SIDE_POWER") { /* milestone 2: power off */ }
      else if (btn == "FRONT_1")    ui.goBack();
      else if (btn == "FRONT_2")    ui.activate();
      else if (btn == "FRONT_3")    ui.moveSelection(-ui.visibleRows);
      else if (btn == "FRONT_4")    ui.moveSelection(ui.visibleRows);
      dirty = true;
    } else if (type == "rescan") {
      std::lock_guard<std::mutex> lk(stateMu);
      ui.rescan();
      dirty = true;
    } else if (type == "orientation") {
      const std::string o = jsonField(payload, "orientation");
      std::lock_guard<std::mutex> lk(stateMu);
      fb.orientation = (o == "landscape") ? Orientation::Landscape : Orientation::Portrait;
      ui.visibleRows = std::max(8, (fb.logicalH() - 130) / 16);
      dirty = true;
    }
  }

  // Per-client thread.
  void serveClient(int fd) {
    auto req = readHttpRequest(fd);
    if (!req) { ::close(fd); return; }

    const std::string upgrade = toLower(req->header("Upgrade"));
    const std::string connectionH = toLower(req->header("Connection"));
    const bool isWs = (upgrade == "websocket") && (connectionH.find("upgrade") != std::string::npos);

    if (req->method == "GET" && req->target == "/ws" && isWs) {
      const std::string key = req->header("Sec-WebSocket-Key");
      if (key.empty()) { sendHttpResponse(fd, 400, "Bad Request", "text/plain", "missing key"); ::close(fd); return; }
      const std::string accept = wsAcceptKey(key);
      std::ostringstream os;
      os << "HTTP/1.1 101 Switching Protocols\r\n"
         << "Upgrade: websocket\r\n"
         << "Connection: Upgrade\r\n"
         << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
      if (!sendAll(fd, os.str().data(), os.str().size())) { ::close(fd); return; }

      {
        std::lock_guard<std::mutex> lk(clientsMu);
        clientFds.push_back(fd);
      }
      log("ws client connected, fd=" + std::to_string(fd));
      sendInitialState(fd);

      while (!shuttingDown) {
        uint8_t opcode;
        std::vector<uint8_t> payload;
        if (!wsRecvFrame(fd, opcode, payload)) break;
        if (opcode == 0x8) { wsSendClose(fd); break; }              // close
        if (opcode == 0x9) { wsSendPong(fd, payload.data(), payload.size()); continue; }  // ping
        if (opcode == 0xA) continue;                                  // pong — ignore
        if (opcode == 0x1) {                                          // text
          std::string s((const char*)payload.data(), payload.size());
          handleClientMessage(s);
        }
        // ignore binary from client
      }

      {
        std::lock_guard<std::mutex> lk(clientsMu);
        clientFds.erase(std::remove(clientFds.begin(), clientFds.end(), fd), clientFds.end());
      }
      log("ws client disconnected, fd=" + std::to_string(fd));
      ::close(fd);
      return;
    }

    // Plain HTTP — serve static
    if (req->method == "GET") {
      const std::string path = resolveStatic(webRoot, req->target);
      if (path.empty()) { sendHttpResponse(fd, 400, "Bad Request", "text/plain", "bad path"); ::close(fd); return; }
      std::string body = readFileToString(path);
      if (body.empty() && !fs::exists(path)) {
        sendHttpResponse(fd, 404, "Not Found", "text/plain", "not found: " + req->target);
      } else {
        sendHttpResponse(fd, 200, "OK", mimeFor(path), body);
      }
      ::close(fd);
      return;
    }

    sendHttpResponse(fd, 405, "Method Not Allowed", "text/plain", "method not allowed");
    ::close(fd);
  }

  // Render thread: rebuilds the framebuffer when state is dirty and broadcasts.
  void renderLoop() {
    while (!shuttingDown) {
      bool wasDirty = dirty.exchange(false);
      if (wasDirty) {
        {
          std::lock_guard<std::mutex> lk(stateMu);
          renderUi(fb, ui);
        }
        broadcastFramebuffer();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
  }

  void run() {
    {
      std::lock_guard<std::mutex> lk(stateMu);
      ui.sdRoot = sdRoot;
      ui.cwd = sdRoot;
      ui.visibleRows = std::max(8, (fb.logicalH() - 130) / 16);
      ui.rescan();
    }
    dirty = true;

    int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) { log("socket() failed"); return; }
    int opt = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(listenFd, (sockaddr*)&addr, sizeof(addr)) < 0) {
      log("bind() failed on port " + std::to_string(port));
      ::close(listenFd);
      return;
    }
    if (::listen(listenFd, 16) < 0) { log("listen() failed"); ::close(listenFd); return; }
    log("AALU emulator (native) listening on http://0.0.0.0:" + std::to_string(port));
    log("sdcard root: " + sdRoot);
    log("web root:    " + webRoot);

    std::thread renderer(&Server::renderLoop, this);

    while (!shuttingDown) {
      sockaddr_in caddr{};
      socklen_t clen = sizeof(caddr);
      int cfd = ::accept(listenFd, (sockaddr*)&caddr, &clen);
      if (cfd < 0) {
        if (errno == EINTR) continue;
        if (shuttingDown) break;
        log("accept() failed");
        continue;
      }
      std::thread(&Server::serveClient, this, cfd).detach();
    }

    ::close(listenFd);
    renderer.join();
  }
};

// ──────────────────────────────────────────────────────────────────────────────

static Server* g_server = nullptr;

static void onSig(int) {
  if (g_server) g_server->shuttingDown = true;
}

int main() {
  Server s;
  s.webRoot = cfg::env_or("AALU_WEB_ROOT", "/web");
  s.sdRoot  = cfg::env_or("AALU_SDCARD",   "/sdcard");
  const char* portStr = cfg::env_or("AALU_PORT", "8080");
  s.port = std::atoi(portStr);
  if (s.port <= 0) s.port = cfg::DEFAULT_PORT;

  g_server = &s;
  signal(SIGINT, onSig);
  signal(SIGTERM, onSig);
  signal(SIGPIPE, SIG_IGN);

  s.run();
  return 0;
}
