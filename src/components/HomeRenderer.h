#pragma once

#include <cstdint>
#include <vector>

class GfxRenderer;
struct RecentBook;
struct Rect;

// Rendering primitives for the seek-reader portrait home screen.
//
// Layout (portrait 480x800):
//   * Hero block       : top, full width. Cover left, metadata right (title,
//                        author, progress bar with %).
//   * Divider          : thin horizontal rule between hero and thumbnail row.
//   * Thumbnail row    : 4 small covers below the hero, each ~100x150.
//   * Bottom menu      : 3 tiles - Browse, Stats, Settings.
//
// All draw functions are no-allocation in the hot path (snprintf into stack
// buffers; Bitmap decode is the only heap activity, mirroring existing themes).
namespace HomeRenderer {

// Constant heights used by HomeActivity to lay out rows. Kept here so the
// activity and the renderer cannot drift apart.
constexpr int kHeroHeight = 300;
constexpr int kThumbnailRowHeight = 150;
constexpr int kBottomMenuHeight = 60;
constexpr int kDividerHeight = 1;

// Heights at which cover thumbnails should be cached. Used both at render time
// (to look up the BMP via UITheme::getCoverThumbPath) and at preload time (so
// HomeActivity can call epub.generateThumbBmp() with matching sizes).
constexpr int kHeroCoverHeight = 300;
constexpr int kThumbnailCoverHeight = 150;

// Each row draw fn renders only the *unselected* state so that the activity
// can store the framebuffer once, then on every focus change just restore the
// buffer and overlay a selection border. This keeps navigation snappy on
// E-ink without re-decoding covers.

// Hero: cover on the left, metadata column on the right.
// `progressPercent` may be HomeProgressCache::Unknown (-1); if so, no progress
// bar is drawn and the layout reflows the metadata vertically.
void drawHero(GfxRenderer& renderer, const Rect& rect, const RecentBook& book, int8_t progressPercent);

// Empty hero: drawn when there are no recent books at all. Shows a "no recent
// book" message and points the user at Browse.
void drawHeroEmpty(GfxRenderer& renderer, const Rect& rect);

// Thin horizontal rule between hero and thumbnail row.
void drawDivider(GfxRenderer& renderer, const Rect& rect);

// Thumbnail row: up to 4 covers, centered. `books` should contain only the
// thumbnails (typically recents 1..4 with recents[0] reserved for the hero).
void drawThumbnailRow(GfxRenderer& renderer, const Rect& rect, const std::vector<RecentBook>& books);

// Bottom menu: 3 fixed tiles (Browse, Stats, Settings), all unselected.
void drawBottomMenu(GfxRenderer& renderer, const Rect& rect);

// Draws a 2-pixel double border around `inner` to mark focus. Inner should be
// the rect of the focused element (cover / thumbnail / menu tile).
void drawSelectionBorder(GfxRenderer& renderer, const Rect& inner);

// Geometry helpers - the activity uses these to figure out where the focused
// element actually sits on screen so it can call drawSelectionBorder.
Rect getHeroCoverRect(const Rect& heroRect);
Rect getThumbnailRect(const Rect& thumbRowRect, int index, int totalCount);
Rect getMenuTileRect(const Rect& menuRect, int index);

}  // namespace HomeRenderer
