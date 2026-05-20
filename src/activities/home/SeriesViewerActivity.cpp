#include "SeriesViewerActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "stats/ReadingStatsManager.h"

namespace {
// Cover dimensions mirror the home-screen thumbnail row (140 x 210) so the
// series viewer reads as the same visual language -- larger, more legible
// covers at the cost of one extra row of scrolling for big series. The 20 px
// inter-cover gap also matches HomeRenderer's kThumbGap, so 3 x 160 cells
// span the full 480 px portrait width with the covers visually breathing.
constexpr int kCellW = 160;
// Cell content (8 top pad + 210 cover + 6 gap + 2-line title @ ~14 px =
// 252 px). kCellH = 260 fits 2 rows under the Lyra header (avail = 627,
// 627/260 = 2 with 107 px slack) and leaves an inter-row gap. Bumped from
// 205 so the larger covers actually get the room they need; the trade-off
// is 6 visible tiles instead of 9.
constexpr int kCellH = 260;
constexpr int kCoverW = 140;
constexpr int kCoverH = 210;
constexpr int kCoverPadTop = 8;
constexpr int kLabelGap = 6;
// Bottom safe margin: keep the grid 10 px clear of the button-hint row so a
// title that wraps to two lines never visually butts against the footer
// chrome (POLISH-SERIES-VIEWER, finding 1 in UX_REDESIGN §2.2.2).
constexpr int kFooterSafeMargin = 10;

// Status of a single book inside the series. Drives the per-cover badge
// shown by drawStatusBadge: a check for finished, a small play-triangle
// "▶ N" for the book currently in progress, and a plain "N" for unread.
enum class BookStatus { Unread, Reading, Finished };

BookStatus statusForBook(const RecentBook& book) {
  if (book.path.empty()) return BookStatus::Unread;
  for (uint8_t i = 0; i < StatsManager.getBookCount(); ++i) {
    const BookStatEntry& entry = StatsManager.getBook(i);
    if (std::strncmp(entry.bookPath, book.path.c_str(), sizeof(entry.bookPath)) == 0) {
      if (entry.progressPercent >= 95) return BookStatus::Finished;
      if (entry.progressPercent > 0) return BookStatus::Reading;
      return BookStatus::Unread;
    }
  }
  return BookStatus::Unread;
}

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

// Two-stroke checkmark inscribed in a circle of `radius`, used as the
// "finished" status glyph. Mirrors the hero's drawCheckmark style so the two
// indicators read as one design language.
void drawCheckGlyph(const GfxRenderer& renderer, int cx, int cy, int radius) {
  if (radius <= 3) return;
  const float scale = static_cast<float>(radius) * 0.75f;
  const int leftX = cx - static_cast<int>(scale * 0.55f);
  const int leftY = cy + static_cast<int>(scale * 0.05f);
  const int vertexX = cx - static_cast<int>(scale * 0.10f);
  const int vertexY = cy + static_cast<int>(scale * 0.45f);
  const int rightX = cx + static_cast<int>(scale * 0.65f);
  const int rightY = cy - static_cast<int>(scale * 0.45f);
  const int thickness = std::max(2, radius / 6);
  renderer.drawLine(leftX, leftY, vertexX, vertexY, thickness, /*state=*/false);
  renderer.drawLine(vertexX, vertexY, rightX, rightY, thickness, /*state=*/false);
}

// Filled right-pointing play triangle, used as the "currently reading" glyph
// next to the index number. Drawn in white (state=false) on the black badge.
void drawPlayTriangle(const GfxRenderer& renderer, int cx, int cy, int size) {
  if (size <= 2) return;
  const int x[3] = {cx - size / 2, cx + size / 2, cx - size / 2};
  const int y[3] = {cy - size / 2, cy, cy + size / 2};
  renderer.fillPolygon(x, y, 3, /*state=*/false);
}

// State-aware top-right badge. POLISH-SERIES-VIEWER (UX_REDESIGN §2.2.2):
// replace the bare series-index disc with a 3-state glyph so the user can
// scan a series and see at a glance which books are done, which is in
// progress, and which are still to read.
void drawStatusBadge(const GfxRenderer& renderer, int coverX, int coverY, const std::string& seriesIndex,
                     int fallbackPosition, BookStatus status) {
  if (status == BookStatus::Finished) {
    // Pure check disc -- no number; the "done" signal is the whole point.
    const int radius = 11;
    const int cx = coverX + kCoverW - radius - 2;
    const int cy = coverY + radius + 2;
    fillDisc(renderer, cx, cy, radius, /*black=*/true);
    drawCheckGlyph(renderer, cx, cy, radius);
    return;
  }

  char buf[8];
  formatBadgeLabel(seriesIndex, fallbackPosition, buf, sizeof(buf));
  const int textW = renderer.getTextWidth(SMALL_FONT_ID, buf);
  const int textH = renderer.getLineHeight(SMALL_FONT_ID);

  if (status == BookStatus::Reading) {
    // Wider pill: play triangle + index. The triangle prefix makes the
    // currently-reading book pop without needing colour or stroke weight.
    constexpr int kTriSize = 7;
    constexpr int kGap = 3;
    const int contentW = kTriSize + kGap + textW;
    const int diameter = std::max(textH + 6, contentW + 10);
    const int radius = diameter / 2;
    const int cx = coverX + kCoverW - radius - 2;
    const int cy = coverY + radius + 2;
    fillDisc(renderer, cx, cy, radius, /*black=*/true);
    const int contentLeft = cx - contentW / 2;
    drawPlayTriangle(renderer, contentLeft + kTriSize / 2, cy, kTriSize);
    const int textX = contentLeft + kTriSize + kGap;
    const int textY = cy - textH / 2;
    renderer.drawText(SMALL_FONT_ID, textX, textY, buf, /*black=*/false);
    return;
  }

  // Unread: plain number disc, unchanged from the previous design so the
  // existing visual language for "book in the series" is preserved.
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
  freeCoverBuffer();
  lastScrollRow = -1;
  lastFocusIndex = -1;
  sortAndPreload();
  requestUpdate();
}

void SeriesViewerActivity::onExit() {
  freeCoverBuffer();
  Activity::onExit();
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

  // Force-refresh cover thumbnails so missing/corrupt covers regenerate.
  // HomeActivity only generates thumbs for the top recents, so series
  // members discovered later often arrive here without a 210 px thumb on
  // disk -- which is what produces the empty outline tiles the user sees.
  // Deleting the existing thumb first ensures stale files get redone too.
  const int total = static_cast<int>(books.size());
  for (int i = 0; i < total; ++i) {
    const RecentBook& book = books[i];
    if (book.coverBmpPath.empty()) continue;
    GUI.fillPopupProgress(renderer, popupRect, (i + 1) * 100 / std::max(total, 1));
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, HomeRenderer::kThumbnailCoverHeight);
    Storage.remove(thumbPath.c_str());
    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      epub.load(false, true);
      epub.generateThumbBmp(HomeRenderer::kThumbnailCoverHeight);
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      if (xtc.load()) {
        xtc.generateThumbBmp(HomeRenderer::kThumbnailCoverHeight);
      }
    }
  }

  focusIndex = 0;
  scrollRow = 0;
  freeCoverBuffer();
  lastScrollRow = -1;
  lastFocusIndex = -1;
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

int SeriesViewerActivity::rowCount() const { return (static_cast<int>(books.size()) + kCols - 1) / kCols; }

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
  // POLISH-SERIES-VIEWER finding 1: keep a small clear band between the grid
  // and the button-hint row so 2-line titles never visually butt against the
  // footer chrome.
  return renderer.getScreenHeight() - metrics.buttonHintsHeight - kFooterSafeMargin;
}

int SeriesViewerActivity::visibleRows() const {
  const int avail = gridBottomY() - gridTopY();
  if (avail <= 0) return 0;
  // Per-row pixels actually consumed by content (cover + label gap + 2 title
  // lines). Smaller than kCellH because kCellH used to include the inter-row
  // gap baked in; the gap is now derived dynamically by rowStride().
  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentH = kCoverPadTop + kCoverH + kLabelGap + titleLineH * 2;
  const int rows = avail / contentH;
  return rows < 1 ? 1 : rows;
}

int SeriesViewerActivity::rowStride() const {
  const int avail = gridBottomY() - gridTopY();
  const int vis = visibleRows();
  if (vis <= 0) return kCellH;
  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentH = kCoverPadTop + kCoverH + kLabelGap + titleLineH * 2;
  // Distribute the available vertical space evenly across visible rows so the
  // inter-row gap absorbs the slack rather than leaving a blank band below
  // the bottom row. Never let the stride drop below content height.
  const int even = avail / vis;
  return even > contentH ? even : contentH;
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

  // Focus-only fast path: if the cached snapshot still matches the visible
  // rows (scrollRow unchanged), just restore it and stamp the new focus
  // border. This skips 6 BMP opens + decodes from SD per key press, which is
  // what made the viewer feel laggy.
  if (coverBufferStored && scrollRow == lastScrollRow && restoreCoverBuffer()) {
    drawFocusBorder();
    lastFocusIndex = focusIndex;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int screenW = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  // Custom header: centered title (vs Lyra's default left-aligned). We still
  // call drawHeader with a nullptr title so the battery icon + background
  // fill come for free; the divider + centered title are drawn manually.
  const Rect headerRect{0, metrics.topPadding, screenW, metrics.headerHeight};
  GUI.drawHeader(renderer, headerRect, nullptr);
  const int titleMaxW = screenW - metrics.batteryWidth * 2 - metrics.contentSidePadding * 2;
  const std::string truncatedTitle =
      renderer.truncatedText(UI_12_FONT_ID, seriesTitle.c_str(), titleMaxW, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, headerRect.y + metrics.batteryBarHeight + 3, truncatedTitle.c_str(),
                            /*black=*/true, EpdFontFamily::BOLD);
  renderer.drawLine(headerRect.x, headerRect.y + headerRect.height - 3, headerRect.x + headerRect.width - 1,
                    headerRect.y + headerRect.height - 3, 3, true);

  // Subtitle: "Author · N books · M finished". The author and finished count
  // are free signal currently missing from the screen -- the author is often
  // shared across every book in a series (so it's both a useful header *and*
  // a confidence cue that the right series loaded), and the finished count
  // is "your place" at a glance. POLISH-SERIES-VIEWER (UX_REDESIGN §2.2.2).
  const int contentTop = metrics.topPadding + metrics.headerHeight + 8;
  int finishedCount = 0;
  for (const auto& b : books) {
    if (statusForBook(b) == BookStatus::Finished) ++finishedCount;
  }
  char subtitleBuf[160];
  const std::string* authorRef = nullptr;
  for (const auto& b : books) {
    if (!b.author.empty()) {
      authorRef = &b.author;
      break;
    }
  }
  if (authorRef != nullptr) {
    std::snprintf(subtitleBuf, sizeof(subtitleBuf), "%s · %d books · %d finished", authorRef->c_str(),
                  static_cast<int>(books.size()), finishedCount);
  } else {
    std::snprintf(subtitleBuf, sizeof(subtitleBuf), "%d books · %d finished", static_cast<int>(books.size()),
                  finishedCount);
  }
  // Truncate the subtitle to the screen width so a long author + finished
  // suffix can't run off the edge. Tighter than the full width because the
  // scroll-up arrow sits to the right of the subtitle on the same baseline
  // and we don't want the two to collide.
  const std::string subtitle = renderer.truncatedText(UI_10_FONT_ID, subtitleBuf, screenW - 48);
  renderer.drawCenteredText(UI_10_FONT_ID, contentTop, subtitle.c_str(), /*black=*/true);

  // Grid layout. 3 columns, vertically scrolled when the series has more
  // rows than fit on screen. Only books inside the visible window are
  // rendered; everything else is clipped at the row level.
  const int gridTop = gridTopY();
  const int gridWidth = kCols * kCellW;
  const int gridLeft = (screenW - gridWidth) / 2;
  const int vis = visibleRows();

  const int total = static_cast<int>(books.size());
  for (int i = 0; i < total; ++i) {
    const int row = rowOf(i);
    if (row < scrollRow || row >= scrollRow + vis) continue;
    const int col = colOf(i);
    const int cellX = gridLeft + col * kCellW;
    const int cellY = gridTop + (row - scrollRow) * rowStride();

    const int coverX = cellX + (kCellW - kCoverW) / 2;
    const int coverY = cellY + kCoverPadTop;
    drawTileCover(renderer, coverX, coverY, books[i]);
    const BookStatus status = statusForBook(books[i]);
    drawStatusBadge(renderer, coverX, coverY, books[i].seriesIndex, /*fallbackPosition=*/i + 1, status);

    // Title under the cover, wrapped to at most 2 lines and centred. Two-line
    // wrap (vs single-line truncation) preserves the trailing phrase that
    // disambiguates same-prefix titles in a series (e.g. "Carl's Doomsday
    // Scenario" vs "Carl's Bedtime Story"). POLISH-SERIES-VIEWER finding 2.
    const int labelY = coverY + kCoverH + kLabelGap;
    const int labelMaxW = kCellW - 8;
    const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const std::vector<std::string> titleLines =
        renderer.wrappedText(UI_10_FONT_ID, books[i].title.c_str(), labelMaxW, /*maxLines=*/2);
    int lineY = labelY;
    for (const auto& line : titleLines) {
      const int titleW = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
      renderer.drawText(UI_10_FONT_ID, cellX + (kCellW - titleW) / 2, lineY, line.c_str());
      lineY += titleLineHeight;
    }
  }

  // Scroll affordances: solid arrow glyphs anchored to the right edge of the
  // screen, vertically aligned with the chrome rows -- up-arrow on the
  // subtitle/author baseline, down-arrow on the button-hint baseline. That
  // puts each arrow on the row of the thing it scrolls "toward", instead of
  // floating in the gap between grid and chrome where it reads as a stray
  // dot. Drawn as filled triangles (12 x 8 px); a 1-bit glyph inside a
  // 4 px disc was unreadable as an arrow on E-ink.
  constexpr int kArrowHalfW = 6;  // triangle base half-width
  constexpr int kArrowH = 8;      // triangle height (tip to base)
  const int uiLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int arrowCx = screenW - kArrowHalfW - 8;  // 8 px breathing room from screen edge
  const int rows = rowCount();
  if (scrollRow > 0) {
    // Up-arrow on the subtitle baseline: centred on the visual middle of the
    // subtitle line so the user reads "scroll up to see more above" at the
    // same row as the header chrome.
    const int subtitleMidY = contentTop + uiLineH / 2;
    const int baseY = subtitleMidY + kArrowH / 2;
    const int tipY = baseY - kArrowH;
    const int x[3] = {arrowCx, arrowCx + kArrowHalfW, arrowCx - kArrowHalfW};
    const int y[3] = {tipY, baseY, baseY};
    renderer.fillPolygon(x, y, 3, /*state=*/true);
  }
  if (scrollRow + vis < rows) {
    // Down-arrow on the button-hint baseline. Sits to the right of the last
    // button (which ends ~24 px from the right edge in portrait), so the
    // button chrome -- drawn later -- can't repaint over it.
    const int buttonMidY = renderer.getScreenHeight() - metrics.buttonHintsHeight / 2;
    const int baseY = buttonMidY - kArrowH / 2;
    const int tipY = baseY + kArrowH;
    const int x[3] = {arrowCx - kArrowHalfW, arrowCx + kArrowHalfW, arrowCx};
    const int y[3] = {baseY, baseY, tipY};
    renderer.fillPolygon(x, y, 3, /*state=*/true);
  }

  GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_OPEN), nullptr, nullptr);

  // Snapshot the framebuffer BEFORE the focus border is drawn. Subsequent
  // focus-only moves restore this buffer and stamp a fresh border on top,
  // skipping the SD reads / BMP decodes that dominate full-render cost.
  coverBufferStored = storeCoverBuffer();
  lastScrollRow = scrollRow;
  lastFocusIndex = focusIndex;

  drawFocusBorder();

  renderer.displayBuffer();
}

void SeriesViewerActivity::drawFocusBorder() const {
  if (books.empty()) return;
  const int screenW = renderer.getScreenWidth();
  const int gridTop = gridTopY();
  const int gridWidth = kCols * kCellW;
  const int gridLeft = (screenW - gridWidth) / 2;
  const int row = rowOf(focusIndex);
  const int col = colOf(focusIndex);
  const int cellX = gridLeft + col * kCellW;
  const int cellY = gridTop + (row - scrollRow) * rowStride();
  const int coverX = cellX + (kCellW - kCoverW) / 2;
  const int coverY = cellY + kCoverPadTop;
  // Square selection border around the cover, matching the home screen.
  renderer.drawRect(coverX - 2, coverY - 2, kCoverW + 4, kCoverH + 4);
  renderer.drawRect(coverX - 3, coverY - 3, kCoverW + 6, kCoverH + 6);
}

bool SeriesViewerActivity::storeCoverBuffer() {
  uint8_t* fb = renderer.getFrameBuffer();
  if (fb == nullptr) return false;
  const size_t sz = renderer.getBufferSize();
  if (sz == 0) return false;
  if (coverBuffer == nullptr) {
    coverBuffer = static_cast<uint8_t*>(malloc(sz));
    if (coverBuffer == nullptr) {
      LOG_ERR("SVW", "coverBuffer malloc failed: %u bytes", static_cast<unsigned>(sz));
      return false;
    }
  }
  memcpy(coverBuffer, fb, sz);
  return true;
}

bool SeriesViewerActivity::restoreCoverBuffer() {
  if (coverBuffer == nullptr) return false;
  uint8_t* fb = renderer.getFrameBuffer();
  if (fb == nullptr) return false;
  const size_t sz = renderer.getBufferSize();
  if (sz == 0) return false;
  memcpy(fb, coverBuffer, sz);
  return true;
}

void SeriesViewerActivity::freeCoverBuffer() {
  if (coverBuffer != nullptr) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}
