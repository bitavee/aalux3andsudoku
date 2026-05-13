#include "HomeRenderer.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/HomeProgressCache.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "stats/ReadingStatsManager.h"

namespace {

constexpr int kHeroPadding = 20;
constexpr int kHeroCoverWidth = 200;
constexpr int kHeroMetaGap = 20;

constexpr int kThumbWidth = 100;
constexpr int kThumbHeight = 150;
constexpr int kThumbGap = 16;
constexpr int kThumbsCount = 4;

constexpr int kMenuPadding = 16;
constexpr int kMenuGap = 8;
constexpr int kMenuTilesCount = 4;

// Draws 0..2 "ghost" book outlines back-up-right of the primary cover so a
// series stack reads as a Kindle-style pile. Each ghost is drawn before the
// primary cover, so the cover paints over their inner edge and the books
// look layered. On a 1-bit panel we don't have grayscale to fade them, so
// each ghost is a single-pixel border with a white interior -- clean
// silhouette without ghosting risk. 4 px offset per layer is wide enough to
// be unmistakable at thumbnail size without clipping the row above.
void drawBackStack(GfxRenderer& renderer, int x, int y, int width, int height, int depth) {
  if (depth <= 0) return;
  if (depth > 2) depth = 2;
  for (int g = depth; g >= 1; --g) {
    const int dx = 4 * g;
    const int dy = -4 * g;
    const int gx = x + dx;
    const int gy = y + dy;
    renderer.fillRect(gx, gy, width, height, /*black=*/false);
    renderer.drawRect(gx, gy, width, height);
  }
}

// Brute-force filled disc. Cheap at badge sizes (radius < 16 px) and avoids
// pulling in an extra GfxRenderer primitive just for one badge variant.
void fillDisc(const GfxRenderer& renderer, int cx, int cy, int radius, bool black) {
  if (radius <= 0) return;
  const int rsq = radius * radius;
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (dx * dx + dy * dy <= rsq) {
        renderer.drawPixel(cx + dx, cy + dy, black);
      }
    }
  }
}

// Round badge with a small number on the top-right of a cover. Used on the
// home thumbnail row to mark a series stack with the total book count, and
// inside the SeriesViewer to show each book's series index. The badge is a
// solid black disc with the number rendered in white -- mirroring the
// progress ribbon's visual language so the two never look like unrelated
// indicators.
void drawRoundCountBadge(const GfxRenderer& renderer, int coverX, int coverY, int coverW, const char* text) {
  if (!text || !*text) return;

  const int textW = renderer.getTextWidth(SMALL_FONT_ID, text);
  const int textH = renderer.getLineHeight(SMALL_FONT_ID);
  // Diameter sized to comfortably hold up to two digits. The +6 padding
  // keeps "1" and "12" both readable without re-layout per cover.
  const int diameter = std::max(textH + 6, textW + 8);
  const int radius = diameter / 2;
  const int cx = coverX + coverW - radius - 2;
  const int cy = coverY + radius + 2;

  fillDisc(renderer, cx, cy, radius, /*black=*/true);

  const int textX = cx - textW / 2;
  // SMALL_FONT_ID's drawText takes y as the top of the ascender box; nudge
  // up by a hair to keep the digit visually centred in the disc.
  const int textY = cy - textH / 2;
  renderer.drawText(SMALL_FONT_ID, textX, textY, text, /*black=*/false);
}

// Draws the cover for a book. Tries the cached thumbnail at `targetHeight`,
// falls back to a bordered placeholder if the thumbnail is not yet available.
// 1-bit thumbs are stretched non-uniformly to fill the slot exactly so the
// cover never leaves a white margin on the right when its source aspect
// ratio doesn't match the home layout's 2:3 thumbnail boxes.
bool drawCover(GfxRenderer& renderer, int x, int y, int width, int height, const RecentBook& book, int targetHeight) {
  if (book.coverBmpPath.empty()) {
    renderer.drawRect(x, y, width, height);
    return false;
  }

  const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, targetHeight);
  FsFile file;
  if (!Storage.openFileForRead("HOME", thumbPath, file)) {
    renderer.drawRect(x, y, width, height);
    return false;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    renderer.drawRect(x, y, width, height);
    return false;
  }

  if (bitmap.is1Bit()) {
    renderer.drawBitmapStretched1Bit(bitmap, x, y, width, height);
  } else {
    renderer.drawBitmap(bitmap, x, y, width, height);
  }
  renderer.drawRect(x, y, width, height);
  file.close();
  return true;
}

// Approximate human-friendly duration (e.g. "6h", "1h 12m", "47m", "<1m").
// Used by the hero "time-left" line where exact precision is noise but the
// at-a-glance shape ("hours" vs "minutes") drives the decision to crack the
// book open.
void formatTimeApprox(char* buf, size_t len, uint64_t ms) {
  const uint32_t totalMin = static_cast<uint32_t>(ms / 60000ULL);
  const uint32_t hours = totalMin / 60;
  const uint32_t mins = totalMin % 60;
  if (hours >= 1) {
    if (mins >= 1 && hours < 10) {
      std::snprintf(buf, len, "%uh %um", static_cast<unsigned>(hours), static_cast<unsigned>(mins));
    } else {
      std::snprintf(buf, len, "%uh", static_cast<unsigned>(hours));
    }
  } else if (totalMin >= 1) {
    std::snprintf(buf, len, "%um", static_cast<unsigned>(totalMin));
  } else {
    std::snprintf(buf, len, "<1m");
  }
}

// Looks up the stats entry for `book` by exact bookPath match. ReadingStatsManager
// stores at most STATS_MAX_BOOK_ENTRIES (=9) books, so this is a cheap linear scan;
// no caching needed.
const BookStatEntry* findStatsByPath(const std::string& bookPath) {
  if (bookPath.empty()) return nullptr;
  const uint8_t count = StatsManager.getBookCount();
  for (uint8_t i = 0; i < count; ++i) {
    const BookStatEntry& entry = StatsManager.getBook(i);
    if (std::strncmp(entry.bookPath, bookPath.c_str(), sizeof(entry.bookPath)) == 0) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace

namespace HomeRenderer {

void drawHero(GfxRenderer& renderer, const Rect& rect, const RecentBook& book, int8_t progressPercent) {
  const Rect coverRect = getHeroCoverRect(rect);
  drawCover(renderer, coverRect.x, coverRect.y, coverRect.width, coverRect.height, book, kHeroCoverHeight);

  const int metaX = coverRect.x + coverRect.width + kHeroMetaGap;
  const int metaWidth = rect.x + rect.width - kHeroPadding - metaX;
  if (metaWidth <= 0) {
    return;
  }

  // Typographic hierarchy per POLISH-HERO (UX_REDESIGN §2.2.1):
  //   Title  -- BOOKERLY_18 BOLD (XL)   : visual anchor, up to 2 wrapped lines.
  //   Author -- UI_10 regular (M)        : secondary identity.
  //   Series -- UI_10 italic (M-italic)  : tertiary, optional.
  // Three sizes / weights instead of three roughly-equal siblings, so the eye
  // lands on the title first.
  const int titleLineHeight = renderer.getLineHeight(BOOKERLY_18_FONT_ID);
  const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  int textY = coverRect.y + 6;
  if (!book.title.empty()) {
    // Wrapping (not truncating) lets two-word titles like "Iron Flame" sit on
    // one line while longer ones break naturally. Cap at 2 lines so the rest
    // of the metadata column (author, series, progress bar, time-left) still
    // fits inside the 300 px hero band.
    const std::vector<std::string> titleLines =
        renderer.wrappedText(BOOKERLY_18_FONT_ID, book.title.c_str(), metaWidth, /*maxLines=*/2, EpdFontFamily::BOLD);
    for (const auto& line : titleLines) {
      renderer.drawText(BOOKERLY_18_FONT_ID, metaX, textY, line.c_str(), /*black=*/true, EpdFontFamily::BOLD);
      textY += titleLineHeight + 2;
    }
    textY += 4;  // breath between title and author
  }

  if (!book.author.empty()) {
    const std::string author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), metaWidth);
    renderer.drawText(UI_10_FONT_ID, metaX, textY, author.c_str());
    textY += authorLineHeight + 4;
  }

  if (!book.seriesName.empty()) {
    // Series name on its own line, "Book N" stacked below it. Combining them
    // on a single line was clipping the index off the right edge for any
    // series with a long name (e.g. "The Empyrean · Book 2" → "The Empyrean
    // · Book…"). Stacking is also closer to the visual hierarchy a reader
    // expects: the *series* is the broader frame, the *index* sits inside it.
    // Both lines are italic to read as one typographic block, distinct from
    // the author line above.
    const std::string seriesLine =
        renderer.truncatedText(UI_10_FONT_ID, book.seriesName.c_str(), metaWidth, EpdFontFamily::ITALIC);
    renderer.drawText(UI_10_FONT_ID, metaX, textY, seriesLine.c_str(), /*black=*/true, EpdFontFamily::ITALIC);
    textY += authorLineHeight + 2;

    if (!book.seriesIndex.empty()) {
      char bookBuf[32];
      std::snprintf(bookBuf, sizeof(bookBuf), "Book %s", book.seriesIndex.c_str());
      const std::string bookLine =
          renderer.truncatedText(UI_10_FONT_ID, bookBuf, metaWidth, EpdFontFamily::ITALIC);
      renderer.drawText(UI_10_FONT_ID, metaX, textY, bookLine.c_str(), /*black=*/true, EpdFontFamily::ITALIC);
      textY += authorLineHeight + 10;
    } else {
      textY += 8;
    }
  } else {
    textY += 8;
  }

  if (progressPercent < 0) {
    return;
  }

  // Linear progress bar -- replaces the circular ring (POLISH-HERO).
  // Less ink, less ghosting risk on E-ink, and (most importantly) leaves room
  // for the secondary "time-left" line below that answers "should I crack
  // this open right now?".
  constexpr int kBarHeight = 10;
  constexpr int kLabelGapPx = 8;
  const int clampedPct = (progressPercent > 100) ? 100 : static_cast<int>(progressPercent);
  char percentStr[8];
  std::snprintf(percentStr, sizeof(percentStr), "%d%%", clampedPct);
  const int labelW = renderer.getTextWidth(UI_10_FONT_ID, percentStr, EpdFontFamily::BOLD);
  const int barW = std::max(0, metaWidth - labelW - kLabelGapPx);

  // Keep the bar / time-left block inside the hero band; if the title wrapped
  // to two lines on a particularly tall font, fall back to the cover bottom
  // rather than overflowing onto the divider below.
  const int heroBottom = coverRect.y + coverRect.height - 4;
  if (textY + kBarHeight > heroBottom) {
    return;
  }

  const int barY = textY;
  if (barW > 0) {
    renderer.drawRect(metaX, barY, barW, kBarHeight);
    const int filledW = (barW - 2) * clampedPct / 100;
    if (filledW > 0) {
      renderer.fillRect(metaX + 1, barY + 1, filledW, kBarHeight - 2, /*state=*/true);
    }
    const int labelTextY = barY + (kBarHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2 - 1;
    renderer.drawText(UI_10_FONT_ID, metaX + barW + kLabelGapPx, labelTextY, percentStr, /*black=*/true,
                      EpdFontFamily::BOLD);
  }
  textY = barY + kBarHeight + 8;

  // Time-left block, drawn as two stacked lines so each fact reads cleanly
  // at a glance: "~Xh left" answers "how much more?" and "Yh read" answers
  // "how invested am I already?". Only shown when (a) the user has put real
  // time into this book, and (b) it isn't already finished. The pace
  // estimate is intentionally per-book (not a global average) so a slow
  // re-read of a technical tome reports more remaining time than a sprint
  // through a beach-read of the same length.
  if (clampedPct <= 0 || clampedPct >= 100) {
    return;
  }
  const BookStatEntry* stat = findStatsByPath(book.path);
  if (!stat || stat->totalReadingMs == 0) {
    return;
  }
  const uint32_t remainingPct = 100u - static_cast<uint32_t>(clampedPct);
  const uint64_t remainingMs =
      static_cast<uint64_t>(stat->totalReadingMs) * remainingPct / static_cast<uint32_t>(clampedPct);

  char leftBuf[32];
  formatTimeApprox(leftBuf, sizeof(leftBuf), remainingMs);
  char readBuf[32];
  formatTimeApprox(readBuf, sizeof(readBuf), stat->totalReadingMs);

  const int timeLineH = renderer.getLineHeight(UI_10_FONT_ID);
  if (textY + timeLineH > heroBottom) {
    return;
  }
  char leftLine[48];
  std::snprintf(leftLine, sizeof(leftLine), "%s left", leftBuf);
  const std::string leftTrunc = renderer.truncatedText(UI_10_FONT_ID, leftLine, metaWidth);
  renderer.drawText(UI_10_FONT_ID, metaX, textY, leftTrunc.c_str());
  textY += timeLineH + 2;

  if (textY + timeLineH > heroBottom) {
    return;
  }
  char readLine[48];
  std::snprintf(readLine, sizeof(readLine), "%s read", readBuf);
  const std::string readTrunc = renderer.truncatedText(UI_10_FONT_ID, readLine, metaWidth);
  renderer.drawText(UI_10_FONT_ID, metaX, textY, readTrunc.c_str());
}

void drawHeroEmpty(GfxRenderer& renderer, const Rect& rect) {
  const int boxX = rect.x + kHeroPadding;
  const int boxY = rect.y;
  const int boxWidth = rect.width - kHeroPadding * 2;
  const int boxHeight = kHeroHeight;

  renderer.drawRect(boxX, boxY, boxWidth, boxHeight);

  const int titleY = boxY + boxHeight / 2 - renderer.getLineHeight(UI_12_FONT_ID);
  renderer.drawCenteredText(UI_12_FONT_ID, titleY, tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);

  const int hintY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 8;
  renderer.drawCenteredText(UI_10_FONT_ID, hintY, tr(STR_BROWSE_TO_OPEN));
}

void drawSectionLabel(GfxRenderer& renderer, const Rect& rect) {
  // drawText takes y as the top of the ascender box and the baseline lands
  // at y + ascender. "Recent Reads" has no descenders, so the visible ink
  // sits between cap-line and baseline -- roughly the lower 70% of the
  // ascender box. Centring on the ascender height alone leaves the ink
  // looking low. Shift up by ~ascender/4 so the visible glyphs are centred.
  const int ascender = renderer.getTextHeight(UI_12_FONT_ID);
  const int textY = rect.y + (rect.height - ascender) / 2 - ascender / 8;
  renderer.drawCenteredText(UI_12_FONT_ID, textY, tr(STR_RECENT_READS), /*black=*/true, EpdFontFamily::BOLD);
}

void drawDivider(GfxRenderer& renderer, const Rect& rect) {
  // Double-line divider with the section label sandwiched between them, so
  // the lines must sit far enough apart for the label to breathe.
  // Extends to the same horizontal bounds as the thumbnail rows and the
  // bottom menu (16 px inset) so the divider visually frames the lower
  // composition rather than only the hero block.
  constexpr int kHalfSeparation = 18;
  constexpr int kDividerSideInset = kMenuPadding;  // = 16, same as menu/thumb row inset
  const int x1 = rect.x + kDividerSideInset;
  const int x2 = rect.x + rect.width - kDividerSideInset;
  renderer.drawLine(x1, rect.y - kHalfSeparation, x2, rect.y - kHalfSeparation);
  renderer.drawLine(x1, rect.y + kHalfSeparation, x2, rect.y + kHalfSeparation);
}

// Black bookmark-ribbon overlay anchored to the top-right of the cover, with
// a V-notch on the bottom edge and the percentage rendered in white. Matches
// the Kindle progress badge affordance.
static void drawProgressBadge(const GfxRenderer& renderer, int x, int y, int width, int /*height*/, int8_t percent) {
  if (percent < 0) return;

  char buf[8];
  std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(percent));

  const int textW = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int textH = renderer.getLineHeight(SMALL_FONT_ID);
  constexpr int padX = 2;
  constexpr int padY = 1;
  constexpr int notchDepth = 3;
  const int badgeW = textW + padX * 2;
  const int badgeH = textH + padY * 2 + notchDepth;

  // Anchor to the top-right corner of the cover.
  const int bx = x + width - badgeW;
  const int by = y;

  // Polygon: top-left -> top-right -> bottom-right -> center notch -> bottom-left.
  const int xPoints[5] = {bx, bx + badgeW, bx + badgeW, bx + badgeW / 2, bx};
  const int yPoints[5] = {by, by, by + badgeH, by + badgeH - notchDepth, by + badgeH};

  renderer.fillPolygon(xPoints, yPoints, 5, /*state=*/true);  // black ribbon

  // White text centred in the rectangular portion above the notch.
  const int textX = bx + (badgeW - textW) / 2;
  const int textY = by + padY;
  renderer.drawText(SMALL_FONT_ID, textX, textY, buf, /*black=*/false);
}

void drawThumbnailRow(GfxRenderer& renderer, const Rect& rect, const std::vector<ThumbTileView>& tiles) {
  const int count = static_cast<int>(tiles.size());
  for (int i = 0; i < count && i < kThumbsCount; ++i) {
    const ThumbTileView& tile = tiles[i];
    if (!tile.book) continue;
    const Rect thumbRect = getThumbnailRect(rect, i, count);
    const int ghostDepth = std::min(2, tile.stackSize - 1);
    drawBackStack(renderer, thumbRect.x, thumbRect.y, thumbRect.width, thumbRect.height, ghostDepth);
    drawCover(renderer, thumbRect.x, thumbRect.y, thumbRect.width, thumbRect.height, *tile.book, kThumbnailCoverHeight);

    if (tile.stackSize > 1) {
      // Series tile: replace the per-book progress ribbon with a round
      // badge showing the total book count. Per-book progress for a stack
      // is meaningless (which book are we measuring?), and the count is
      // the more useful at-a-glance signal.
      char countBuf[8];
      std::snprintf(countBuf, sizeof(countBuf), "%d", tile.stackSize);
      drawRoundCountBadge(renderer, thumbRect.x, thumbRect.y, thumbRect.width, countBuf);
    } else {
      const int8_t percent = HomeProgressCache::getInstance().getProgress(tile.book->path);
      // Skip the badge for unread books (0%) so the row doesn't get peppered
      // with "0%" ribbons that carry no information; the absence of a badge
      // is itself the "not started" signal.
      if (percent > 0) {
        drawProgressBadge(renderer, thumbRect.x, thumbRect.y, thumbRect.width, thumbRect.height, percent);
      }
    }
  }
}

void drawBottomMenu(GfxRenderer& renderer, const Rect& rect) {
  const char* labels[kMenuTilesCount] = {tr(STR_FILES), tr(STR_STATS_TITLE), tr(STR_TRANSFER), tr(STR_SETTINGS_TITLE)};

  // Top-only rounded corners, matching the side button hints on the file
  // browser page (see LyraTheme::drawSideButtonHints).
  constexpr int kCornerRadius = 6;

  for (int i = 0; i < kMenuTilesCount; ++i) {
    const Rect tile = getMenuTileRect(rect, i);
    renderer.drawRoundedRect(tile.x, tile.y, tile.width, tile.height, 1, kCornerRadius,
                             /*roundTopLeft=*/true, /*roundTopRight=*/true,
                             /*roundBottomLeft=*/false, /*roundBottomRight=*/false, true);

    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
    const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textX = tile.x + (tile.width - textWidth) / 2;
    const int textY = tile.y + (tile.height - textHeight) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, labels[i]);
  }
}

void drawSelectionBorder(GfxRenderer& renderer, const Rect& inner, bool rTL, bool rTR, bool rBL, bool rBR) {
  const bool anyRounded = rTL || rTR || rBL || rBR;
  if (!anyRounded) {
    // Cheap path for cover/thumbnail focus - those are rectangular.
    renderer.drawRect(inner.x - 2, inner.y - 2, inner.width + 4, inner.height + 4);
    renderer.drawRect(inner.x - 3, inner.y - 3, inner.width + 6, inner.height + 6);
    return;
  }

  // Concentric rounded outlines that follow the host tile's corner shape.
  // 6 px matches the corner radius used by drawBottomMenu.
  constexpr int kCornerRadius = 6;
  renderer.drawRoundedRect(inner.x - 2, inner.y - 2, inner.width + 4, inner.height + 4, 1, kCornerRadius, rTL, rTR, rBL,
                           rBR, true);
  renderer.drawRoundedRect(inner.x - 3, inner.y - 3, inner.width + 6, inner.height + 6, 1, kCornerRadius, rTL, rTR, rBL,
                           rBR, true);
}

Rect getHeroCoverRect(const Rect& heroRect) {
  return Rect{heroRect.x + kHeroPadding, heroRect.y, kHeroCoverWidth, kHeroHeight};
}

Rect getThumbnailRect(const Rect& thumbRowRect, int index, int /*totalCount*/) {
  const int total = (kThumbsCount * kThumbWidth) + ((kThumbsCount - 1) * kThumbGap);
  const int rowX = thumbRowRect.x + (thumbRowRect.width - total) / 2;
  return Rect{rowX + index * (kThumbWidth + kThumbGap), thumbRowRect.y, kThumbWidth, kThumbHeight};
}

Rect getMenuTileRect(const Rect& menuRect, int index) {
  const int totalGaps = kMenuGap * (kMenuTilesCount - 1);
  const int tileWidth = (menuRect.width - kMenuPadding * 2 - totalGaps) / kMenuTilesCount;
  return Rect{menuRect.x + kMenuPadding + index * (tileWidth + kMenuGap), menuRect.y, tileWidth, menuRect.height};
}

}  // namespace HomeRenderer
