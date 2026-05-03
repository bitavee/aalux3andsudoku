#include "HomeRenderer.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {

constexpr int kHeroPadding = 20;
constexpr int kHeroCoverWidth = 200;
constexpr int kHeroMetaGap = 20;
constexpr int kProgressBarHeight = 12;

constexpr int kThumbWidth = 100;
constexpr int kThumbHeight = 150;
constexpr int kThumbGap = 16;
constexpr int kThumbsCount = 4;

constexpr int kMenuPadding = 16;
constexpr int kMenuGap = 8;
constexpr int kMenuTilesCount = 4;

// Draws the cover for a book. Tries the cached thumbnail at `targetHeight`,
// falls back to a bordered placeholder if the thumbnail is not yet available.
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

  renderer.drawBitmap(bitmap, x, y, width, height);
  renderer.drawRect(x, y, width, height);
  file.close();
  return true;
}

void drawProgressBar(const GfxRenderer& renderer, int x, int y, int width, int height, int8_t percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  renderer.drawRect(x, y, width, height);
  const int fillWidth = (width - 4) * percent / 100;
  if (fillWidth > 0) {
    renderer.fillRect(x + 2, y + 2, fillWidth, height - 4);
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
    textY += authorLineHeight + 14;
  } else {
    textY += 14;
  }

  if (progressPercent >= 0) {
    drawProgressBar(renderer, metaX, textY, metaWidth, kProgressBarHeight, progressPercent);
    textY += kProgressBarHeight + 6;

    char percentStr[8];
    std::snprintf(percentStr, sizeof(percentStr), "%d%%", static_cast<int>(progressPercent));
    renderer.drawText(UI_10_FONT_ID, metaX, textY, percentStr);
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

void drawDivider(GfxRenderer& renderer, const Rect& rect) {
  renderer.drawLine(rect.x + kHeroPadding, rect.y, rect.x + rect.width - kHeroPadding, rect.y);
}

void drawThumbnailRow(GfxRenderer& renderer, const Rect& rect, const std::vector<RecentBook>& books) {
  const int count = static_cast<int>(books.size());
  for (int i = 0; i < count && i < kThumbsCount; ++i) {
    const Rect thumbRect = getThumbnailRect(rect, i, count);
    drawCover(renderer, thumbRect.x, thumbRect.y, thumbRect.width, thumbRect.height, books[i], kThumbnailCoverHeight);
  }
}

void drawBottomMenu(GfxRenderer& renderer, const Rect& rect) {
  const char* labels[kMenuTilesCount] = {tr(STR_FILES), tr(STR_STATS_TITLE), tr(STR_TRANSFER),
                                          tr(STR_SETTINGS_TITLE)};

  for (int i = 0; i < kMenuTilesCount; ++i) {
    const Rect tile = getMenuTileRect(rect, i);
    renderer.drawRect(tile.x, tile.y, tile.width, tile.height);

    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, labels[i]);
    const int textHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int textX = tile.x + (tile.width - textWidth) / 2;
    const int textY = tile.y + (tile.height - textHeight) / 2;
    renderer.drawText(UI_10_FONT_ID, textX, textY, labels[i]);
  }
}

void drawSelectionBorder(GfxRenderer& renderer, const Rect& inner) {
  // Two concentric outlines outside `inner` so we don't paint over cover art.
  renderer.drawRect(inner.x - 2, inner.y - 2, inner.width + 4, inner.height + 4);
  renderer.drawRect(inner.x - 3, inner.y - 3, inner.width + 6, inner.height + 6);
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
