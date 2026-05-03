#include "HomeRenderer.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/HomeProgressCache.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {

constexpr int kHeroPadding = 20;
constexpr int kHeroCoverWidth = 200;
constexpr int kHeroMetaGap = 20;
constexpr int kRingThickness = 12;

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

// Donut-style progress indicator. The full ring is outlined regardless of
// progress; the progress portion is filled clockwise starting at 12 o'clock.
// This is a brute-force pass over the bounding box (one atan2 per ring pixel),
// which is fast enough on E-ink even at ~80 px outer radius - the heavy work
// only runs when the home re-renders, not on every selection change.
void drawProgressRing(const GfxRenderer& renderer, int cx, int cy, int outerR, int ringThickness, int8_t percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  const int innerR = std::max(0, outerR - ringThickness);
  const int outerRsq = outerR * outerR;
  const int innerRsq = innerR * innerR;
  const int outerRimSq = (outerR - 1) * (outerR - 1);  // 1 px outer rim
  const int innerRimSq = innerR > 0 ? innerR * innerR : 0;
  const int innerRimInnerSq = innerR > 1 ? (innerR - 1) * (innerR - 1) : 0;
  const float endAngleDeg = percent * 3.6f;

  for (int dy = -outerR; dy <= outerR; ++dy) {
    for (int dx = -outerR; dx <= outerR; ++dx) {
      const int distSq = dx * dx + dy * dy;
      if (distSq > outerRsq) continue;

      // Always draw both rims so the empty portion of the ring still reads
      // as a circle rather than vanishing on a 0%-progress book.
      const bool isOuterRim = distSq > outerRimSq && distSq <= outerRsq;
      const bool isInnerRim = innerR > 0 && distSq > innerRimInnerSq && distSq <= innerRimSq;
      if (isOuterRim || isInnerRim) {
        renderer.drawPixel(cx + dx, cy + dy, true);
        continue;
      }

      // Filled portion is the area between rims; gate by clockwise angle.
      if (distSq <= innerRsq) continue;

      // 12 o'clock = (0, -outerR), clockwise positive: angle = atan2(dx, -dy)
      float angle = atan2f(static_cast<float>(dx), static_cast<float>(-dy)) * 180.0f / static_cast<float>(M_PI);
      if (angle < 0) angle += 360.0f;
      if (angle <= endAngleDeg) {
        renderer.drawPixel(cx + dx, cy + dy, true);
      }
    }
  }
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

  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, book.title.c_str(), metaWidth, EpdFontFamily::BOLD);
  int textY = coverRect.y + 8;
  renderer.drawText(UI_12_FONT_ID, metaX, textY, title.c_str(), true, EpdFontFamily::BOLD);
  textY += titleLineHeight + 6;

  if (!book.author.empty()) {
    const std::string author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), metaWidth);
    renderer.drawText(UI_10_FONT_ID, metaX, textY, author.c_str());
    textY += authorLineHeight + 6;
  }

  if (!book.seriesName.empty()) {
    // "Series Name" or "Series Name · Book N" -- truncated to fit the
    // metadata column. Drawn in italic UI_10 to differentiate from the author
    // line above. The separator and "Book" label are intentionally English-
    // only for now since they're typographic glue rather than translatable
    // copy; if this proves wrong, wrap with tr().
    char buf[160];
    if (!book.seriesIndex.empty()) {
      std::snprintf(buf, sizeof(buf), "%s · Book %s", book.seriesName.c_str(), book.seriesIndex.c_str());
    } else {
      std::snprintf(buf, sizeof(buf), "%s", book.seriesName.c_str());
    }
    const std::string seriesLine = renderer.truncatedText(UI_10_FONT_ID, buf, metaWidth, EpdFontFamily::ITALIC);
    renderer.drawText(UI_10_FONT_ID, metaX, textY, seriesLine.c_str(), true, EpdFontFamily::ITALIC);
    textY += authorLineHeight + 8;
  } else {
    textY += 8;
  }

  if (progressPercent >= 0) {
    // Drop the progress ring into the empty space below the title/author so
    // the right column doesn't sit half-empty. The ring is sized to the
    // smaller of the column width and the remaining vertical room, with a
    // small inset so it never collides with the cover edge or the hero
    // border.
    constexpr int kRingMargin = 8;
    const int availableHeight = (coverRect.y + coverRect.height) - textY - kRingMargin;
    const int availableWidth = metaWidth - kRingMargin * 2;
    int diameter = std::min(availableWidth, availableHeight);
    if (diameter > 0) {
      // Cap the visual size so big screens don't produce an absurdly thick
      // donut; the cover is the visual anchor, not the progress indicator.
      diameter = std::min(diameter, 180);
      const int outerR = diameter / 2;
      const int cx = metaX + metaWidth / 2;
      const int cy = textY + (availableHeight) / 2;

      drawProgressRing(renderer, cx, cy, outerR, kRingThickness, progressPercent);

      // Centered percent label inside the ring.
      char percentStr[8];
      std::snprintf(percentStr, sizeof(percentStr), "%d%%", static_cast<int>(progressPercent));
      const int labelWidth = renderer.getTextWidth(BOOKERLY_18_FONT_ID, percentStr, EpdFontFamily::BOLD);
      const int labelHeight = renderer.getLineHeight(BOOKERLY_18_FONT_ID);
      renderer.drawText(BOOKERLY_18_FONT_ID, cx - labelWidth / 2, cy - labelHeight / 2 - 2, percentStr, true,
                        EpdFontFamily::BOLD);
    }
  }
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
    drawCover(renderer, thumbRect.x, thumbRect.y, thumbRect.width, thumbRect.height, *tile.book,
              kThumbnailCoverHeight);

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
  const char* labels[kMenuTilesCount] = {tr(STR_FILES), tr(STR_STATS_TITLE), tr(STR_TRANSFER),
                                          tr(STR_SETTINGS_TITLE)};

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
  renderer.drawRoundedRect(inner.x - 2, inner.y - 2, inner.width + 4, inner.height + 4, 1, kCornerRadius, rTL, rTR,
                           rBL, rBR, true);
  renderer.drawRoundedRect(inner.x - 3, inner.y - 3, inner.width + 6, inner.height + 6, 1, kCornerRadius, rTL, rTR,
                           rBL, rBR, true);
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
