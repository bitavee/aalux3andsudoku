#include "BookshelfActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SeriesGrouping.h"
#include "SeriesViewerActivity.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderUtils.h"  // for GO_HOME_MS
#include "activities/util/ConfirmationActivity.h"
#include "components/GrayscaleCoverPass.h"
#include "components/HomeProgressCache.h"
#include "components/HomeRenderer.h"  // kThumbnailCoverHeight, drawStackedCover, drawBottomButtonHints
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Tile geometry mirrors SeriesViewer so the bookshelf reads as the same
// visual component at scale -- covers, gaps and title slot identical.
constexpr int kCols = 3;
constexpr int kCellW = 160;
constexpr int kCellH = 260;
constexpr int kCoverW = 140;
constexpr int kCoverH = 210;
constexpr int kCoverPadTop = 8;
constexpr int kLabelGap = 6;
constexpr int kFooterSafeMargin = 10;
constexpr int kMaxBookshelfBooks = 300;

// Compute the thumbnail BMP path for an entry. The Bookshelf cache stores
// only the source path on disk to keep RAM tight; the thumb location is a
// deterministic function of the path (Epub/Xtc use the same hash but
// different cache-directory prefixes), so we recompute here at tile build
// time rather than persisting the string.
std::string thumbPathFor(const std::string& path) {
  const char* prefix = FsHelpers::hasXtcExtension(path) ? "xtc_" : "epub_";
  char buf[96];
  std::snprintf(buf, sizeof(buf), "/.crosspoint/%s%llu/thumb_%d.bmp", prefix,
                static_cast<unsigned long long>(FsHelpers::cachePathHash(path)), HomeRenderer::kThumbnailCoverHeight);
  return buf;
}

// True when two entries belong in the same bookshelf tile. The bookshelf
// shows the whole library, so we ONLY group by explicit series metadata --
// the folder-fallback used by Home (where same-folder books pile into one
// recents tile) would collapse hundreds of standalone books in one folder
// into a single giant stack here, which is exactly the bug we hit.
bool entriesGroup(const BookshelfCache::Entry& a, const BookshelfCache::Entry& b) {
  return SeriesGrouping::sameSeries(a.seriesName, b.seriesName);
}

// Label for a series stack. Stacks are only formed when both seed and
// every other member share a non-empty seriesName, so the name is always
// available here.
std::string stackLabel(const BookshelfCache::Entry& seed) { return seed.seriesName; }

}  // namespace

void BookshelfActivity::onEnter() {
  Activity::onEnter();
  loadOrScan();
  requestUpdate();
}

void BookshelfActivity::onExit() {
  Activity::onExit();
  freeCoverBuffer();
  HomeProgressCache::getInstance().clear();
  entries.clear();
  tiles.clear();
  tiles.shrink_to_fit();
  entries.shrink_to_fit();
}

void BookshelfActivity::loadOrScan() {
  if (BookshelfCache::exists() && BookshelfCache::load(entries)) {
    buildTiles();
    mode = entries.empty() ? Mode::Empty : Mode::Grid;
    return;
  }

  // Cold cache: scan synchronously. Show the popup first so the user knows
  // something is happening before we start hitting the SD card.
  runScan();
}

void BookshelfActivity::runScan() {
  mode = Mode::Loading;
  freeCoverBuffer();
  lastScrollRow = -1;
  lastFocusIndex = -1;

  renderer.clearScreen();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_REFRESHING_LIBRARY));
  GUI.fillPopupProgress(renderer, popupRect, 0);
  renderer.displayBuffer();

  entries.clear();
  BookshelfCache::scan(renderer, popupRect, entries, kMaxBookshelfBooks);
  buildTiles();

  focusIndex = 0;
  scrollRow = 0;
  mode = entries.empty() ? Mode::Empty : Mode::Grid;

  // First-time refresh tooltip: fires once after the first successful scan
  // that produces books. Standalone empty state has its own messaging; no
  // tooltip when there's nothing to refresh against yet.
  if (mode == Mode::Grid && !SETTINGS.bookshelfRefreshHintSeen) {
    tooltipPending = true;
  }
}

void BookshelfActivity::buildTiles() {
  tiles.clear();
  tiles.reserve(entries.size());

  const int n = static_cast<int>(entries.size());
  std::vector<uint8_t> consumed(n, 0);

  for (int i = 0; i < n; ++i) {
    if (consumed[i]) continue;
    Tile tile;
    tile.entryIndices.push_back(i);
    consumed[i] = 1;

    // Greedy forward scan: because the cache is sorted by author -> series
    // -> index, every member of a given group sits in a contiguous run.
    // Walk forward, absorb adjacent entries that still group with the seed,
    // stop on the first non-grouping entry.
    for (int j = i + 1; j < n; ++j) {
      if (consumed[j]) continue;
      if (!entriesGroup(entries[i], entries[j])) break;
      tile.entryIndices.push_back(j);
      consumed[j] = 1;
    }

    const BookshelfCache::Entry& seed = entries[tile.entryIndices.front()];
    if (tile.entryIndices.size() > 1) {
      tile.label = stackLabel(seed);
    } else {
      tile.label = seed.title;
    }
    tile.thumbPath = thumbPathFor(seed.path);
    tiles.push_back(std::move(tile));
  }

  LOG_DBG("BSH", "buildTiles: %d entries -> %zu tiles", n, tiles.size());
}

bool BookshelfActivity::isFocusOnSeries() const {
  if (tiles.empty()) return false;
  const int idx = std::clamp(focusIndex, 0, static_cast<int>(tiles.size()) - 1);
  return tiles[idx].entryIndices.size() > 1;
}

int BookshelfActivity::rowCount() const { return (static_cast<int>(tiles.size()) + kCols - 1) / kCols; }

int BookshelfActivity::rowOf(int idx) const { return idx / kCols; }

int BookshelfActivity::colOf(int idx) const { return idx % kCols; }

int BookshelfActivity::gridTopY() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
}

int BookshelfActivity::gridBottomY() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  return renderer.getScreenHeight() - metrics.buttonHintsHeight - kFooterSafeMargin;
}

int BookshelfActivity::visibleRows() const {
  const int avail = gridBottomY() - gridTopY();
  if (avail <= 0) return 0;
  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentH = kCoverPadTop + kCoverH + kLabelGap + titleLineH * 2;
  const int rows = avail / contentH;
  return rows < 1 ? 1 : rows;
}

int BookshelfActivity::rowStride() const {
  const int avail = gridBottomY() - gridTopY();
  const int vis = visibleRows();
  if (vis <= 0) return kCellH;
  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentH = kCoverPadTop + kCoverH + kLabelGap + titleLineH * 2;
  const int even = avail / vis;
  return even > contentH ? even : contentH;
}

void BookshelfActivity::clampFocus() {
  const int total = static_cast<int>(tiles.size());
  if (total == 0) {
    focusIndex = 0;
    return;
  }
  if (focusIndex >= total) focusIndex = total - 1;
  if (focusIndex < 0) focusIndex = 0;
}

void BookshelfActivity::clampScroll() {
  const int rows = rowCount();
  const int vis = visibleRows();
  if (rows <= vis) {
    scrollRow = 0;
    return;
  }
  const int focusRow = rowOf(focusIndex);
  if (focusRow < scrollRow) scrollRow = focusRow;
  if (focusRow >= scrollRow + vis) scrollRow = focusRow - vis + 1;
  if (scrollRow < 0) scrollRow = 0;
  if (scrollRow > rows - vis) scrollRow = rows - vis;
}

void BookshelfActivity::loop() {
  if (mode == Mode::Loading) return;

  // First-time tooltip swallows the next input release: any button dismisses
  // the modal, persists the seen flag, and re-renders the grid.
  if (tooltipPending) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      tooltipPending = false;
      SETTINGS.bookshelfRefreshHintSeen = 1;
      SETTINGS.saveToFile();
      freeCoverBuffer();
      lastScrollRow = -1;
      lastFocusIndex = -1;
      requestUpdate(true);
    }
    return;
  }

  // Long-press Back = re-scan. Short-press Back = goHome.
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
      runScan();
      requestUpdate(true);
      return;
    }
    onGoHome();
    return;
  }

  if (mode != Mode::Grid) return;

  const int total = static_cast<int>(tiles.size());
  bool changed = false;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Long-press Confirm on a single-book tile deletes the book.
    if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
      if (!isFocusOnSeries()) {
        confirmDeleteFocused();
        return;
      }
      // Long-press on a series stack falls through to short-press semantics
      // (drill in). Deleting a whole series wasn't part of the design.
    }
    openFocused();
    return;
  }

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
  }

  if (changed) {
    clampScroll();
    requestUpdate();
  }
}

void BookshelfActivity::openFocused() {
  if (tiles.empty()) return;
  clampFocus();
  const Tile& tile = tiles[focusIndex];

  // Series stack: drill into SeriesViewer with our in-RAM subset. We already
  // have every member; no need to walk SD or consult per-series cache.
  if (tile.entryIndices.size() > 1) {
    std::vector<RecentBook> books;
    books.reserve(tile.entryIndices.size());
    for (int idx : tile.entryIndices) {
      const BookshelfCache::Entry& e = entries[idx];
      RecentBook rb;
      rb.path = e.path;
      rb.title = e.title;
      rb.seriesName = e.seriesName;
      rb.seriesIndex = e.seriesIndex;
      // RecentBook::coverBmpPath is by convention the thumb-path template
      // with the literal "[HEIGHT]" placeholder (see SeriesGrouping.cpp:306
      // and Epub::getThumbBmpPath()). UITheme::getCoverThumbPath substitutes
      // [HEIGHT] for the requested cover height to land on thumb_N.bmp.
      // Passing a bare cover.bmp here (no placeholder) caused SeriesViewer
      // to open the un-resized cover -- which the bookshelf scan never
      // generates -- so every drill-in showed empty rects.
      const char* prefix = FsHelpers::hasXtcExtension(e.path) ? "xtc_" : "epub_";
      char coverBuf[96];
      std::snprintf(coverBuf, sizeof(coverBuf), "/.crosspoint/%s%llu/thumb_[HEIGHT].bmp", prefix,
                    static_cast<unsigned long long>(FsHelpers::cachePathHash(e.path)));
      rb.coverBmpPath = coverBuf;
      books.push_back(std::move(rb));
    }
    activityManager.pushActivity(
        std::make_unique<SeriesViewerActivity>(renderer, mappedInput, tile.label, std::move(books)));
    return;
  }

  // Single book: existence pre-check. If the file is gone, offer to
  // refresh the library so the user can clean up the stale tile.
  const std::string path = entries[tile.entryIndices.front()].path;
  if (!Storage.exists(path.c_str())) {
    handleMissingBook(path);
    return;
  }
  onSelectBook(path);
}

void BookshelfActivity::handleMissingBook(const std::string& path) {
  LOG_DBG("BSH", "Confirm on missing book: %s", path.c_str());

  // Prompt the user: book not found -> refresh? Yes runs the scan, No
  // returns to the grid. ConfirmationActivity provides the Y/N affordance
  // for free; we just dispatch on the result.
  auto handler = [this](const ActivityResult& res) {
    if (!res.isCancelled) {
      runScan();
    }
    requestUpdate(true);
  };
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_BOOK_NOT_FOUND), tr(STR_REFRESH_LIBRARY)),
      std::move(handler));
}

void BookshelfActivity::confirmDeleteFocused() {
  if (tiles.empty()) return;
  if (isFocusOnSeries()) return;
  const Tile& tile = tiles[focusIndex];
  const std::string path = entries[tile.entryIndices.front()].path;
  const std::string title = entries[tile.entryIndices.front()].title;

  auto handler = [this, path](const ActivityResult& res) {
    if (res.isCancelled) {
      requestUpdate(true);
      return;
    }
    LOG_DBG("BSH", "Deleting %s", path.c_str());
    // Clear the per-book cache directory (cover, sections, progress) so a
    // re-add of the same file gets a fresh cache, then remove the EPUB and
    // patch the bookshelf cache so the tile vanishes from the grid.
    if (FsHelpers::hasEpubExtension(path)) {
      // Epub::clearCache removes /.crosspoint/epub_<hash>/.
      Epub(path, "/.crosspoint").clearCache();
    }
    Storage.remove(path.c_str());
    BookshelfCache::removeBook(path);

    // Drop the entry from the in-RAM list and rebuild tiles in place so
    // the user sees an immediate update without a full re-scan.
    auto it =
        std::find_if(entries.begin(), entries.end(), [&](const BookshelfCache::Entry& e) { return e.path == path; });
    if (it != entries.end()) entries.erase(it);
    buildTiles();

    if (entries.empty()) {
      mode = Mode::Empty;
      focusIndex = 0;
      scrollRow = 0;
    } else if (focusIndex >= static_cast<int>(tiles.size())) {
      focusIndex = static_cast<int>(tiles.size()) - 1;
    }
    freeCoverBuffer();
    lastScrollRow = -1;
    lastFocusIndex = -1;
    requestUpdate(true);
  };

  const std::string heading = tr(STR_DELETE) + std::string("? ");
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, title),
                         std::move(handler));
}

void BookshelfActivity::render(RenderLock&&) {
  if (mode == Mode::Loading) {
    // Popup is already on-screen from runScan(); just leave it. A render
    // pass during Loading mode is unexpected (scan is synchronous) but we
    // guard against it for safety.
    renderer.displayBuffer();
    return;
  }

  if (mode == Mode::Empty) {
    renderEmpty();
    return;
  }

  renderGrid();

  if (tooltipPending) {
    showFirstTimeTooltip();
  }
}

void BookshelfActivity::renderEmpty() {
  renderer.clearScreen();

  const int screenW = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenW, metrics.headerHeight}, tr(STR_BOOKSHELF));

  // Wrap the message so it doesn't get truncated on narrow orientations.
  const int contentY = renderer.getScreenHeight() / 2 - renderer.getLineHeight(UI_12_FONT_ID);
  const std::vector<std::string> lines =
      renderer.wrappedText(UI_12_FONT_ID, tr(STR_NO_BOOKSHELF_BOOKS), screenW - 40, /*maxLines=*/3);
  int y = contentY;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, line.c_str());
    y += renderer.getLineHeight(UI_12_FONT_ID) + 4;
  }

  const Rect hintRect{0, renderer.getScreenHeight() - HomeRenderer::kButtonHintsHeight, screenW,
                      HomeRenderer::kButtonHintsHeight};
  HomeRenderer::drawBottomButtonHints(renderer, hintRect);

  renderer.displayBuffer();
}

void BookshelfActivity::drawGridCovers() {
  const int screenW = renderer.getScreenWidth();
  const int gridTop = gridTopY();
  const int gridWidth = kCols * kCellW;
  const int gridLeft = (screenW - gridWidth) / 2;
  const int vis = visibleRows();
  const int total = static_cast<int>(tiles.size());
  for (int i = 0; i < total; ++i) {
    const int row = rowOf(i);
    if (row < scrollRow || row >= scrollRow + vis) continue;
    const int col = colOf(i);
    const int cellX = gridLeft + col * kCellW;
    const int cellY = gridTop + (row - scrollRow) * rowStride();
    const int coverX = cellX + (kCellW - kCoverW) / 2;
    const int coverY = cellY + kCoverPadTop;
    const Tile& tile = tiles[i];
    HomeRenderer::drawStackedCover(renderer, coverX, coverY, kCoverW, kCoverH, tile.thumbPath,
                                   static_cast<int>(tile.entryIndices.size()));
    if (tile.entryIndices.size() == 1) {
      const std::string& bookPath = entries[tile.entryIndices.front()].path;
      HomeProgressCache::getInstance().loadProgressFor(bookPath);
      HomeRenderer::drawCoverProgressOverlay(renderer, coverX, coverY, kCoverW, kCoverH,
                                             HomeProgressCache::getInstance().getProgress(bookPath));
    }
  }
}

void BookshelfActivity::renderGrid() {
  clampFocus();
  clampScroll();

  // Focus-only fast path: same scroll window, just stamp the new focus
  // border on top of the cached snapshot.
  if (coverBufferStored && scrollRow == lastScrollRow && restoreCoverBuffer()) {
    drawFocusBorder();
    lastFocusIndex = focusIndex;
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  const int screenW = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, screenW, metrics.headerHeight}, tr(STR_BOOKSHELF));

  const int gridTop = gridTopY();
  const int gridWidth = kCols * kCellW;
  const int gridLeft = (screenW - gridWidth) / 2;
  const int vis = visibleRows();

  const int total = static_cast<int>(tiles.size());
  for (int i = 0; i < total; ++i) {
    const int row = rowOf(i);
    if (row < scrollRow || row >= scrollRow + vis) continue;
    const int col = colOf(i);
    const int cellX = gridLeft + col * kCellW;
    const int cellY = gridTop + (row - scrollRow) * rowStride();

    const int coverX = cellX + (kCellW - kCoverW) / 2;
    const int coverY = cellY + kCoverPadTop;
    const Tile& tile = tiles[i];
    HomeRenderer::drawStackedCover(renderer, coverX, coverY, kCoverW, kCoverH, tile.thumbPath,
                                   static_cast<int>(tile.entryIndices.size()));

    // Individual books get the progress overlay (bottom bar / completed check);
    // series stacks show their count badge only. Progress is looked up lazily
    // for the visible tile so the shelf never parses book.bin for all 300 books.
    if (tile.entryIndices.size() == 1) {
      const std::string& bookPath = entries[tile.entryIndices.front()].path;
      HomeProgressCache::getInstance().loadProgressFor(bookPath);
      HomeRenderer::drawCoverProgressOverlay(renderer, coverX, coverY, kCoverW, kCoverH,
                                             HomeProgressCache::getInstance().getProgress(bookPath));
    }

    // 2-line wrapped title under the cover (or series name for stacks).
    const int labelY = coverY + kCoverH + kLabelGap;
    const int labelMaxW = kCellW - 8;
    const int titleLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const std::vector<std::string> titleLines =
        renderer.wrappedText(UI_10_FONT_ID, tile.label.c_str(), labelMaxW, /*maxLines=*/2);
    int lineY = labelY;
    for (const auto& line : titleLines) {
      const int titleW = renderer.getTextWidth(UI_10_FONT_ID, line.c_str());
      renderer.drawText(UI_10_FONT_ID, cellX + (kCellW - titleW) / 2, lineY, line.c_str());
      lineY += titleLineHeight;
    }
  }

  // Scroll affordances on the right edge so the user knows when more rows
  // exist above or below the visible window.
  constexpr int kArrowHalf = 6;
  constexpr int kArrowH = 8;
  const int arrowCx = screenW - kArrowHalf - 8;
  const int rows = rowCount();
  if (scrollRow > 0) {
    const int baseY = gridTop - 4;
    const int tipY = baseY - kArrowH;
    const int x[3] = {arrowCx, arrowCx + kArrowHalf, arrowCx - kArrowHalf};
    const int y[3] = {tipY, baseY, baseY};
    renderer.fillPolygon(x, y, 3, true);
  }
  if (scrollRow + vis < rows) {
    const int baseY = gridBottomY() + 2;
    const int tipY = baseY + kArrowH;
    const int x[3] = {arrowCx - kArrowHalf, arrowCx + kArrowHalf, arrowCx};
    const int y[3] = {baseY, baseY, tipY};
    renderer.fillPolygon(x, y, 3, true);
  }

  const Rect hintRect{0, renderer.getScreenHeight() - HomeRenderer::kButtonHintsHeight, screenW,
                      HomeRenderer::kButtonHintsHeight};
  HomeRenderer::drawBottomButtonHints(renderer, hintRect);

  lastScrollRow = scrollRow;
  lastFocusIndex = focusIndex;

  drawFocusBorder();

  renderer.displayBuffer();
  renderCoversGrayscale(renderer, [this]() { drawGridCovers(); });
}

void BookshelfActivity::drawFocusBorder() const {
  if (tiles.empty()) return;
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
  renderer.drawRoundedRect(coverX - 2, coverY - 2, kCoverW + 4, kCoverH + 4, 1, HomeRenderer::kCoverCornerRadius + 2,
                           true);
  renderer.drawRoundedRect(coverX - 3, coverY - 3, kCoverW + 6, kCoverH + 6, 1, HomeRenderer::kCoverCornerRadius + 3,
                           true);
  renderer.drawRoundedRect(coverX - 4, coverY - 4, kCoverW + 8, kCoverH + 8, 1, HomeRenderer::kCoverCornerRadius + 4,
                           true);
}

void BookshelfActivity::showFirstTimeTooltip() {
  // Drawn on top of the already-rendered grid: the modal popup explains the
  // long-press refresh gesture. Dismissed by any button press in loop().
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_REFRESH_HINT_BODY));
  (void)popupRect;
  renderer.displayBuffer();
}

bool BookshelfActivity::storeCoverBuffer() {
  uint8_t* fb = renderer.getFrameBuffer();
  if (fb == nullptr) return false;
  const size_t sz = renderer.getBufferSize();
  if (sz == 0) return false;
  if (coverBuffer == nullptr) {
    coverBuffer = static_cast<uint8_t*>(malloc(sz));
    if (coverBuffer == nullptr) {
      LOG_ERR("BSH", "coverBuffer malloc failed: %u bytes", static_cast<unsigned>(sz));
      return false;
    }
  }
  std::memcpy(coverBuffer, fb, sz);
  return true;
}

bool BookshelfActivity::restoreCoverBuffer() {
  if (coverBuffer == nullptr) return false;
  uint8_t* fb = renderer.getFrameBuffer();
  if (fb == nullptr) return false;
  const size_t sz = renderer.getBufferSize();
  if (sz == 0) return false;
  std::memcpy(fb, coverBuffer, sz);
  return true;
}

void BookshelfActivity::freeCoverBuffer() {
  if (coverBuffer != nullptr) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}
