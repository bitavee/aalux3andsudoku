#include "SeriesViewerActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "MappedInputManager.h"
#include "SeriesGrouping.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderUtils.h"  // for GO_HOME_MS
#include "components/HomeProgressCache.h"
#include "components/HomeRenderer.h"  // for kThumbnailCoverHeight
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {
constexpr int kCellW = 140;
// Cell content height (after dropping the italic "Book N" line, which dup'd
// the round badge on the cover) is 188 px: 8 top pad + 150 cover + 6 gap +
// 24 title. kCellH = 209 fits exactly 3 rows under the Lyra header (84 px)
// in portrait (avail = 627 / 3 = 209) with ~21 px between rows. Base theme
// (45 px header) gets 3 rows with even more breathing room.
constexpr int kCellH = 209;
constexpr int kCoverW = 100;
constexpr int kCoverH = 150;
constexpr int kCoverPadTop = 8;
constexpr int kLabelGap = 6;

void drawTileCover(GfxRenderer& renderer, int x, int y, const RecentBook& book) {
  if (book.coverBmpPath.empty()) {
    renderer.drawRect(x, y, kCoverW, kCoverH);
    return;
  }
  const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, HomeRenderer::kThumbnailCoverHeight);
  FsFile file;
  if (!Storage.openFileForRead("SVW", thumbPath, file)) {
    renderer.drawRect(x, y, kCoverW, kCoverH);
    return;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    renderer.drawRect(x, y, kCoverW, kCoverH);
    return;
  }
  if (bitmap.is1Bit()) {
    renderer.drawBitmapStretched1Bit(bitmap, x, y, kCoverW, kCoverH);
  } else {
    renderer.drawBitmap(bitmap, x, y, kCoverW, kCoverH);
  }
  renderer.drawRect(x, y, kCoverW, kCoverH);
  file.close();
}

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

// Format the badge label. Prefers the parsed seriesIndex when available
// ("3" / "1.5"); otherwise falls back to the 1-based position in the sorted
// stack so every book in the viewer carries a number, even when the EPUBs
// only declare a series name without an index.
void formatBadgeLabel(const std::string& seriesIndex, int fallbackPosition, char* buf, size_t bufSize) {
  if (!seriesIndex.empty()) {
    if (seriesIndex.find('.') == std::string::npos) {
      std::snprintf(buf, bufSize, "%s", seriesIndex.c_str());
      return;
    }
    const float v = std::strtof(seriesIndex.c_str(), nullptr);
    if (v == static_cast<int>(v)) {
      std::snprintf(buf, bufSize, "%d", static_cast<int>(v));
    } else {
      std::snprintf(buf, bufSize, "%g", v);
    }
    return;
  }
  std::snprintf(buf, bufSize, "%d", fallbackPosition);
}

// Top-right round badge showing the book's series position.
void drawSeriesIndexBadge(const GfxRenderer& renderer, int coverX, int coverY, const std::string& seriesIndex,
                          int fallbackPosition) {
  char buf[8];
  formatBadgeLabel(seriesIndex, fallbackPosition, buf, sizeof(buf));

  const int textW = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int textH = renderer.getLineHeight(SMALL_FONT_ID);
  const int diameter = std::max(textH + 6, textW + 8);
  const int radius = diameter / 2;
  const int cx = coverX + kCoverW - radius - 2;
  const int cy = coverY + radius + 2;
  fillDisc(renderer, cx, cy, radius, /*black=*/true);
  const int textX = cx - textW / 2;
  const int textY = cy - textH / 2;
  renderer.drawText(SMALL_FONT_ID, textX, textY, buf, /*black=*/false);
}
}  // namespace

namespace {
// Sort key: integer-by-100 so 1.5 sorts after 1.0 and before 2.0. Falls back
// to the first integer found in the filename, then INT_MAX so unkeyed books
// drift to the end. Filename is the secondary tiebreaker.
std::pair<long, std::string> orderKeyFor(const RecentBook& book) {
  const auto slash = book.path.find_last_of('/');
  const std::string name = (slash == std::string::npos) ? book.path : book.path.substr(slash + 1);

  long key = 0x7FFFFFFFL;
  if (!book.seriesIndex.empty()) {
    char* end = nullptr;
    const float f = std::strtof(book.seriesIndex.c_str(), &end);
    if (end != book.seriesIndex.c_str()) {
      key = static_cast<long>(f * 100.0f);
    }
  }
  if (key == 0x7FFFFFFFL) {
    size_t i = 0;
    while (i < name.size() && (name[i] < '0' || name[i] > '9')) ++i;
    if (i < name.size()) {
      long n = 0;
      while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
        n = n * 10 + (name[i] - '0');
        ++i;
        if (n > 1000000) break;
      }
      key = n * 100;
    }
  }
  return {key, name};
}
}  // namespace

void SeriesViewerActivity::sortAndPreload() {
  std::sort(books.begin(), books.end(),
            [](const RecentBook& a, const RecentBook& b) { return orderKeyFor(a) < orderKeyFor(b); });
  for (const auto& b : books) {
    HomeProgressCache::getInstance().loadProgressFor(b.path);
  }
}

void SeriesViewerActivity::onEnter() {
  Activity::onEnter();
  focusIndex = 0;
  scrollRow = 0;
  sortAndPreload();
  requestUpdate();
}

void SeriesViewerActivity::runScan() {
  if (books.empty()) return;
  const std::string key = SeriesGrouping::countCacheKey(books[0]);

  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 0);

  // Re-run the same discovery logic that ran on first viewer open. The
  // current `books` acts as the seed: discoverSeriesMembers walks each
  // unique parent folder and merges in any new candidates that satisfy
  // shouldGroup. Result is then persisted, overwriting the previous cache.
  books = SeriesGrouping::discoverSeriesMembers(std::move(books), renderer, popupRect);
  SeriesGrouping::saveCachedBooks(key, books);

  focusIndex = 0;
  scrollRow = 0;
  sortAndPreload();
  requestUpdate();
}

void SeriesViewerActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Long-press Back = re-scan the series and overwrite the cache.
    // Short-press Back keeps its existing role of popping the viewer.
    if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
      runScan();
      return;
    }
    activityManager.popActivity();
    return;
  }
  if (books.empty()) return;

  const int total = static_cast<int>(books.size());
  bool changed = false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (focusIndex > 0) {
      --focusIndex;
      changed = true;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    if (focusIndex < total - 1) {
      ++focusIndex;
      changed = true;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (focusIndex - kCols >= 0) {
      focusIndex -= kCols;
      changed = true;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (focusIndex + kCols < total) {
      focusIndex += kCols;
      changed = true;
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activityManager.goToReader(books[focusIndex].path);
    return;
  }

  if (changed) {
    clampScroll();
    requestUpdate();
  }
}

int SeriesViewerActivity::rowCount() const {
  return (static_cast<int>(books.size()) + kCols - 1) / kCols;
}

int SeriesViewerActivity::countInRow(int row) const {
  const int total = static_cast<int>(books.size());
  const int startIdx = row * kCols;
  return std::min(kCols, total - startIdx);
}

void SeriesViewerActivity::clampFocus() {
  const int total = static_cast<int>(books.size());
  if (total == 0) {
    focusIndex = 0;
    return;
  }
  if (focusIndex >= total) focusIndex = total - 1;
  if (focusIndex < 0) focusIndex = 0;
}

int SeriesViewerActivity::gridTopY() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int subtitleH = renderer.getLineHeight(UI_10_FONT_ID);
  return metrics.topPadding + metrics.headerHeight + 8 + subtitleH + 12;
}

int SeriesViewerActivity::gridBottomY() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - metrics.buttonHintsHeight;
}

int SeriesViewerActivity::visibleRows() const {
  const int avail = gridBottomY() - gridTopY();
  if (avail <= 0) return 0;
  const int rows = avail / kCellH;
  return rows < 1 ? 1 : rows;
}

void SeriesViewerActivity::clampScroll() {
  const int rows = rowCount();
  const int vis = visibleRows();
  if (rows <= vis) {
    scrollRow = 0;
    return;
  }
  // Keep the focused tile inside the visible window.
  const int focusRow = rowOf(focusIndex);
  if (focusRow < scrollRow) scrollRow = focusRow;
  if (focusRow >= scrollRow + vis) scrollRow = focusRow - vis + 1;
  if (scrollRow < 0) scrollRow = 0;
  if (scrollRow > rows - vis) scrollRow = rows - vis;
}

void SeriesViewerActivity::render(RenderLock&&) {
  clampFocus();
  clampScroll();
  const int screenW = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenW, metrics.headerHeight}, seriesTitle.c_str());

  // Subtitle: "N books"
  const int contentTop = metrics.topPadding + metrics.headerHeight + 8;
  char subtitleBuf[32];
  std::snprintf(subtitleBuf, sizeof(subtitleBuf), "%d books", static_cast<int>(books.size()));
  renderer.drawCenteredText(UI_10_FONT_ID, contentTop, subtitleBuf, /*black=*/true);

  // Grid layout. 3 columns, vertically scrolled when the series has more
  // rows than fit on screen. Only books inside the visible window are
  // rendered; everything else is clipped at the row level.
  const int gridTop = gridTopY();
  const int gridBottom = gridBottomY();
  const int gridWidth = kCols * kCellW;
  const int gridLeft = (screenW - gridWidth) / 2;
  const int vis = visibleRows();

  const int total = static_cast<int>(books.size());
  for (int i = 0; i < total; ++i) {
    const int row = rowOf(i);
    if (row < scrollRow || row >= scrollRow + vis) continue;
    const int col = colOf(i);
    const int cellX = gridLeft + col * kCellW;
    const int cellY = gridTop + (row - scrollRow) * kCellH;

    const int coverX = cellX + (kCellW - kCoverW) / 2;
    const int coverY = cellY + kCoverPadTop;
    drawTileCover(renderer, coverX, coverY, books[i]);
    drawSeriesIndexBadge(renderer, coverX, coverY, books[i].seriesIndex, /*fallbackPosition=*/i + 1);

    // Title under the cover, single-line, centred and truncated. The book's
    // series index is already shown by the round badge on the cover's top
    // right corner -- no need to repeat "Book N" below the title.
    const int labelY = coverY + kCoverH + kLabelGap;
    const int labelMaxW = kCellW - 8;
    const std::string title = renderer.truncatedText(UI_10_FONT_ID, books[i].title.c_str(), labelMaxW);
    const int titleW = renderer.getTextWidth(UI_10_FONT_ID, title.c_str());
    renderer.drawText(UI_10_FONT_ID, cellX + (kCellW - titleW) / 2, labelY, title.c_str());

    if (i == focusIndex) {
      // Square selection border around the cover, matching the home screen.
      renderer.drawRect(coverX - 2, coverY - 2, kCoverW + 4, kCoverH + 4);
      renderer.drawRect(coverX - 3, coverY - 3, kCoverW + 6, kCoverH + 6);
    }
  }

  // Scroll affordances: small arrow markers near the right edge of the
  // grid when there's content above (^) or below (v) the visible window.
  // Cheap, render only when relevant to avoid noise on short series.
  const int rows = rowCount();
  if (scrollRow > 0) {
    const int x = gridLeft + gridWidth - 8;
    const int y = gridTop - 12;
    fillDisc(renderer, x, y, 4, /*black=*/true);
    renderer.drawText(SMALL_FONT_ID, x - 2, y - 4, "^", /*black=*/false);
  }
  if (scrollRow + vis < rows) {
    const int x = gridLeft + gridWidth - 8;
    const int y = gridBottom - 4;
    fillDisc(renderer, x, y, 4, /*black=*/true);
    renderer.drawText(SMALL_FONT_ID, x - 2, y - 4, "v", /*black=*/false);
  }

  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_OPEN), nullptr, nullptr);

  renderer.displayBuffer();
}
