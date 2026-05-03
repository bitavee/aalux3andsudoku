#include "HomeActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SeriesGrouping.h"
#include "SeriesViewerActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/stats/StatsActivity.h"
#include "components/HomeProgressCache.h"
#include "components/HomeRenderer.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"

namespace {
constexpr int kThumbsPerRow = 4;
// The home grid still caps at 1 hero + 8 thumbs (= 9 tiles), but we no
// longer cap how many *recents* we load: a single tile can be a series
// stack containing many books, and the drill-in viewer needs every member
// to be present. Display tile count is enforced separately at render time.
constexpr int kMaxHomeRecents = 0x7FFFFFFF;  // effectively no cap
constexpr int kHeroPaddingTop = 50;       // hero starts immediately under header
// Section break holds the double-line divider with the "Recent Reads" label
// sandwiched between the two rules. The two rows hug 12 px apart with a small
// breath above the menu. 50 + 300 + 64 + 150 + 12 + 150 + 14 + 60 = 800.
constexpr int kHeroBottomGap = 64;
constexpr int kRowGap = 12;
constexpr int kMenuPaddingBottom = 0;     // menu tiles glued to the bottom edge
// Approximate UI_12 line height; used to centre the label vertically between
// the two divider rules (which sit at gap/2 ± kHalfSeparation = 18 px).
constexpr int kSectionLabelHeight = 18;
constexpr int kMenuItemBrowse = 0;
constexpr int kMenuItemStats = 1;
constexpr int kMenuItemTransfer = 2;
constexpr int kMenuItemSettings = 3;
constexpr int kMenuItemCount = 4;
}  // namespace

// ---------- Lifecycle ----------

void HomeActivity::onEnter() {
  Activity::onEnter();

  recentBooks.clear();
  recentsLoaded = false;
  recentsLoading = false;
  firstRenderDone = false;
  coverBufferStored = false;
  progressLoaded = false;
  freeCoverBuffer();

  loadRecentBooks(kMaxHomeRecents);

  if (recentBooks.empty()) {
    focusRow = FOCUS_MENU;
    focusIndex = 0;
  } else {
    focusRow = FOCUS_HERO;
    focusIndex = 0;
  }

  // Pre-load covers and progress so the first render paints a finished
  // home in one shot. Both paths are now fast in the steady state: the
  // reader pre-generates home-sized thumbnails at book open, and
  // HomeProgressCache short-circuits on a persistent {spineIndex, percent}
  // file. On a cold cache, the cover loader shows its own popup; the
  // progress loader is silent because every miss writes its result
  // straight to disk.
  if (!recentBooks.empty()) {
    loadRecentCovers();
    for (const auto& book : recentBooks) {
      HomeProgressCache::getInstance().loadProgressFor(book.path);
    }
    progressLoaded = true;
  }

  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();
  freeCoverBuffer();
  HomeProgressCache::getInstance().clear();
}

void HomeActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openFocused();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    moveFocus(-1, 0);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    moveFocus(+1, 0);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    moveFocus(0, -1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    moveFocus(0, +1);
    return;
  }
}

// ---------- Recent books loading ----------

void HomeActivity::loadRecentBooks(int max) {
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.clear();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), max));

  // Backfill series metadata for entries saved before the series feature
  // shipped: recent.json wrote them with empty seriesName. We try the fast
  // path (book.bin v6 already on disk) and skip silently if it's not there
  // -- the next time the user opens that book it'll populate naturally.
  // Persist any successful backfill so we don't redo this work on every
  // home re-entry.
  std::vector<int> persistIndices;
  for (const RecentBook& book : books) {
    if (static_cast<int>(recentBooks.size()) >= max) break;
    if (!Storage.exists(book.path.c_str())) continue;

    RecentBook entry = book;
    if (entry.seriesName.empty() && FsHelpers::hasEpubExtension(entry.path)) {
      Epub epub(entry.path, "/.crosspoint");
      // buildIfMissing=false: don't trigger a slow re-parse on home entry.
      // Stale (v5) caches simply won't backfill until the book is opened.
      if (epub.load(false, true)) {
        const std::string& s = epub.getSeriesName();
        if (!s.empty()) {
          entry.seriesName = s;
          entry.seriesIndex = epub.getSeriesIndex();
          persistIndices.push_back(static_cast<int>(recentBooks.size()));
        }
      }
    }
    recentBooks.push_back(std::move(entry));
  }

  for (int idx : persistIndices) {
    const RecentBook& b = recentBooks[idx];
    RECENT_BOOKS.updateBook(b.path, b.title, b.author, b.coverBmpPath, b.seriesName, b.seriesIndex);
  }

  buildTiles();
}

namespace {
// Series-grouping helpers (seriesKey, sameSeries, seriesConflict, parentDir,
// shouldGroup) live in SeriesGrouping so the SD-card discovery walker can
// share the same definition of "in this stack" as the home tile builder.

std::string baseName(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// First contiguous run of digits in `s`, parsed as an int. Returns -1 when
// the string contains no digits. Used as a sort key for series members
// whose EPUB metadata lacks an explicit seriesIndex; filenames like
// "Foundation - 03.epub" or "03 - Foundation.epub" sort correctly without
// needing a real natural-sort implementation.
int firstInt(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && (s[i] < '0' || s[i] > '9')) ++i;
  if (i >= s.size()) return -1;
  int n = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    n = n * 10 + (s[i] - '0');
    ++i;
    if (n > 1000000) break;  // guard against pathological filenames
  }
  return n;
}

// Single integer key for sort ordering -- mirrors SeriesViewerActivity's
// orderKey so the home tile's cover (bookIndices[0]) and the viewer's first
// slot always show the same book. Key precedence:
//   1. parsed seriesIndex * 100 ("1" -> 100, "1.5" -> 150);
//   2. first integer found in the filename * 100;
//   3. INT_MAX so unkeyed books drift to the end.
// Filename is the secondary tiebreaker so two books with the same key sort
// consistently across runs.
std::pair<long, std::string> orderKey(const RecentBook& b) {
  const std::string name = baseName(b.path);
  long key = 0x7FFFFFFFL;
  if (!b.seriesIndex.empty()) {
    char* end = nullptr;
    const float f = std::strtof(b.seriesIndex.c_str(), &end);
    if (end != b.seriesIndex.c_str()) {
      key = static_cast<long>(f * 100.0f);
    }
  }
  if (key == 0x7FFFFFFFL) {
    const int ia = firstInt(name);
    if (ia >= 0) {
      key = static_cast<long>(ia) * 100;
    }
  }
  return {key, name};
}

}  // namespace

void HomeActivity::buildTiles() {
  tiles.clear();
  if (recentBooks.empty()) return;

  // Tile 0 is always the hero alone -- never a stack. The hero is the user's
  // "continue reading" anchor, so we don't dilute it with stack visuals or
  // route Confirm into a viewer. Browsing siblings happens via the dedicated
  // series tile below.
  HomeTile heroTile;
  heroTile.bookIndices.push_back(0);
  tiles.push_back(std::move(heroTile));

  // Books absorbed into the hero's series tile (so we don't re-render them as
  // solo thumbs further down).
  std::vector<bool> claimed(recentBooks.size(), false);

  const RecentBook& hero = recentBooks[0];
  const std::string heroDir = SeriesGrouping::parentDir(hero.path);

  // Surface the hero's series as the FIRST thumb tile when it has any
  // siblings -- whether matched by series name, by folder, or by a mix of
  // the two. The hero is appended last in the index list so:
  //   - the thumb cover is a *different* book from the hero (visually
  //     unambiguous: "this is more from the same series");
  //   - the drill-in viewer still contains every series member, with the
  //     non-hero members listed first so the initial focus lands on a
  //     plausible "next read" rather than re-offering the current book.
  if (!hero.seriesName.empty() || !heroDir.empty()) {
    HomeTile seriesTile;
    for (int i = 1; i < static_cast<int>(recentBooks.size()); ++i) {
      if (SeriesGrouping::shouldGroup(hero, recentBooks[i])) {
        seriesTile.bookIndices.push_back(i);
        claimed[i] = true;
      }
    }
    if (!seriesTile.bookIndices.empty()) {
      seriesTile.bookIndices.push_back(0);  // hero is part of the stack too

      // Sort by series order so the thumb cover (bookIndices[0]) and the
      // drill-in viewer's first slot always show the same book -- the one
      // with the lowest series order key.
      std::sort(seriesTile.bookIndices.begin(), seriesTile.bookIndices.end(),
                [&](int a, int b) { return orderKey(recentBooks[a]) < orderKey(recentBooks[b]); });

      tiles.push_back(std::move(seriesTile));
    }
  }

  // Now group whatever's left for the remaining thumbnails. Books already
  // claimed by the hero's series tile are skipped. The hero (index 0) is
  // also skipped because tile 0 already represents it.
  for (int i = 1; i < static_cast<int>(recentBooks.size()); ++i) {
    if (claimed[i]) continue;
    const RecentBook& candidate = recentBooks[i];

    int matchTile = -1;
    // Skip tile 0 (hero alone) and any tile that already contains the hero
    // (the hero-series tile we just built); other tiles are fair game.
    for (int t = 1; t < static_cast<int>(tiles.size()); ++t) {
      const HomeTile& tile = tiles[t];
      if (tile.bookIndices.empty()) continue;
      bool tileContainsHero = false;
      for (int idx : tile.bookIndices) {
        if (idx == 0) {
          tileContainsHero = true;
          break;
        }
      }
      if (tileContainsHero) continue;

      const RecentBook& head = recentBooks[tile.bookIndices[0]];
      if (SeriesGrouping::shouldGroup(head, candidate)) {
        matchTile = t;
        break;
      }
    }

    if (matchTile >= 0) {
      tiles[matchTile].bookIndices.push_back(i);
    } else {
      HomeTile tile;
      tile.bookIndices.push_back(i);
      tiles.push_back(std::move(tile));
    }
  }

  // Apply the persistent series-count cache to any tile that's a stack.
  // The cache is populated by SeriesViewerActivity discovery; until the user
  // has opened the viewer at least once, the badge falls back to the
  // recents-derived count.
  for (HomeTile& tile : tiles) {
    if (tile.bookIndices.size() < 2) continue;
    const RecentBook& head = recentBooks[tile.bookIndices[0]];
    const int cached = SeriesGrouping::lookupSeriesCount(SeriesGrouping::countCacheKey(head));
    if (cached > static_cast<int>(tile.bookIndices.size())) {
      tile.displayStackSize = cached;
    }
  }
}

void HomeActivity::loadRecentCovers() {
  if (recentsLoading) return;
  recentsLoading = true;

  bool showingPopup = false;
  Rect popupRect;
  const int totalBooks = static_cast<int>(recentBooks.size());
  int progress = 0;

  for (int i = 0; i < totalBooks; ++i) {
    RecentBook& book = recentBooks[i];
    if (book.coverBmpPath.empty()) {
      ++progress;
      continue;
    }

    const int targetHeight = (i == 0) ? HomeRenderer::kHeroCoverHeight : HomeRenderer::kThumbnailCoverHeight;
    const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, targetHeight);
    if (Storage.exists(thumbPath.c_str())) {
      ++progress;
      continue;
    }

    if (FsHelpers::hasEpubExtension(book.path)) {
      Epub epub(book.path, "/.crosspoint");
      epub.load(false, true);

      if (!showingPopup) {
        showingPopup = true;
        popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      }
      GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / std::max(totalBooks, 1)));

      if (!epub.generateThumbBmp(targetHeight)) {
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, "", book.seriesName, book.seriesIndex);
        book.coverBmpPath = "";
      }
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      Xtc xtc(book.path, "/.crosspoint");
      if (xtc.load()) {
        if (!showingPopup) {
          showingPopup = true;
          popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
        }
        GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / std::max(totalBooks, 1)));
        if (!xtc.generateThumbBmp(targetHeight)) {
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, "", book.seriesName, book.seriesIndex);
          book.coverBmpPath = "";
        }
      }
    }
    ++progress;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

// ---------- Cover buffer ----------

bool HomeActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;
  freeCoverBuffer();
  const size_t bufferSize = renderer.getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) {
    LOG_ERR("HOME", "malloc failed for cover buffer: %u bytes", bufferSize);
    return false;
  }
  memcpy(coverBuffer, frameBuffer, bufferSize);
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer) return false;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;
  memcpy(frameBuffer, coverBuffer, renderer.getBufferSize());
  return true;
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
}

// ---------- Navigation ----------

bool HomeActivity::rowIsFocusable(FocusRow row) const {
  // Rows are now driven by tiles, not raw book count, so a series stack
  // counts as a single tile regardless of how many books it contains.
  const int tileCount = static_cast<int>(tiles.size());
  switch (row) {
    case FOCUS_HERO:
      return tileCount > 0;
    case FOCUS_THUMBS_R1:
      return tileCount > 1;
    case FOCUS_THUMBS_R2:
      return tileCount > 1 + kThumbsPerRow;
    case FOCUS_MENU:
      return true;
  }
  return false;
}

int HomeActivity::rowItemCount(FocusRow row) const {
  const int tileCount = static_cast<int>(tiles.size());
  switch (row) {
    case FOCUS_HERO:
      return tileCount > 0 ? 1 : 0;
    case FOCUS_THUMBS_R1:
      return std::min(kThumbsPerRow, std::max(0, tileCount - 1));
    case FOCUS_THUMBS_R2:
      return std::min(kThumbsPerRow, std::max(0, tileCount - 1 - kThumbsPerRow));
    case FOCUS_MENU:
      return kMenuItemCount;
  }
  return 0;
}

void HomeActivity::moveFocus(int dRow, int dCol) {
  if (dRow != 0) {
    constexpr FocusRow rowOrder[] = {FOCUS_HERO, FOCUS_THUMBS_R1, FOCUS_THUMBS_R2, FOCUS_MENU};
    constexpr int rowOrderCount = sizeof(rowOrder) / sizeof(rowOrder[0]);

    const int currentCol = focusIndex;
    int currentPos = 0;
    for (int i = 0; i < rowOrderCount; ++i) {
      if (rowOrder[i] == focusRow) {
        currentPos = i;
        break;
      }
    }

    for (int step = 0; step < rowOrderCount; ++step) {
      currentPos = (currentPos + dRow + rowOrderCount) % rowOrderCount;
      const FocusRow candidate = rowOrder[currentPos];
      if (!rowIsFocusable(candidate)) continue;

      // When moving between the two thumbnail rows, keep the column so the
      // focus rides directly up/down. Otherwise reset to col 0.
      const bool wasThumbs = (focusRow == FOCUS_THUMBS_R1 || focusRow == FOCUS_THUMBS_R2);
      const bool isThumbs = (candidate == FOCUS_THUMBS_R1 || candidate == FOCUS_THUMBS_R2);
      focusRow = candidate;
      if (wasThumbs && isThumbs) {
        focusIndex = std::min(currentCol, rowItemCount(candidate) - 1);
      } else {
        focusIndex = 0;
      }
      requestUpdate();
      return;
    }
    return;
  }

  if (dCol != 0) {
    const int count = rowItemCount(focusRow);
    if (count <= 1) return;
    focusIndex = (focusIndex + dCol + count) % count;
    requestUpdate();
  }
}

void HomeActivity::openFocused() {
  // Single-book tile -> open directly. Series stack -> push the viewer so
  // the user can pick which member to read. The viewer's first focus is
  // the most-recently-read member (bookIndices[0]), so "continue reading"
  // is still one extra Confirm away rather than buried in a deep menu.
  auto openTile = [&](int tileIndex) {
    if (tileIndex < 0 || tileIndex >= static_cast<int>(tiles.size())) return;
    const HomeTile& tile = tiles[tileIndex];
    if (tile.bookIndices.empty()) return;

    if (tile.bookIndices.size() == 1) {
      const int bookIdx = tile.bookIndices[0];
      if (bookIdx >= 0 && bookIdx < static_cast<int>(recentBooks.size())) {
        activityManager.goToReader(recentBooks[bookIdx].path);
      }
      return;
    }

    // Build a viewer payload from this tile's books. Series title falls
    // back to the parent folder name when grouping was via folder fallback.
    std::vector<RecentBook> stackBooks;
    stackBooks.reserve(tile.bookIndices.size());
    for (int idx : tile.bookIndices) {
      if (idx < 0 || idx >= static_cast<int>(recentBooks.size())) continue;
      stackBooks.push_back(recentBooks[idx]);
    }
    if (stackBooks.empty()) return;

    std::string title = stackBooks[0].seriesName;
    if (title.empty()) {
      const std::string& path = stackBooks[0].path;
      const auto slash = path.find_last_of('/');
      if (slash != std::string::npos && slash > 0) {
        const auto prevSlash = path.find_last_of('/', slash - 1);
        title = path.substr(prevSlash == std::string::npos ? 0 : prevSlash + 1, slash - (prevSlash + 1));
      }
    }

    // Cache-first: if discovery has run for this series before, reuse the
    // saved book list. Avoids the multi-second SD walk on every viewer open.
    // The viewer's long-press Back lets the user force a refresh.
    const std::string cacheKey = SeriesGrouping::countCacheKey(stackBooks[0]);
    std::vector<RecentBook> cached;
    if (SeriesGrouping::loadCachedBooks(cacheKey, cached)) {
      stackBooks = std::move(cached);
    } else {
      // First time opening this series -- run discovery, persist the result
      // so subsequent opens are instant.
      Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
      stackBooks = SeriesGrouping::discoverSeriesMembers(std::move(stackBooks), renderer, popupRect);
      SeriesGrouping::saveCachedBooks(cacheKey, stackBooks);
    }

    activityManager.pushActivity(
        std::make_unique<SeriesViewerActivity>(renderer, mappedInput, std::move(title), std::move(stackBooks)));
  };

  switch (focusRow) {
    case FOCUS_HERO:
      openTile(0);
      break;
    case FOCUS_THUMBS_R1:
      openTile(focusIndex + 1);  // hero = tiles[0], R1 = tiles[1..4]
      break;
    case FOCUS_THUMBS_R2:
      openTile(focusIndex + 1 + kThumbsPerRow);  // R2 = tiles[5..8]
      break;
    case FOCUS_MENU:
      switch (focusIndex) {
        case kMenuItemBrowse:
          activityManager.goToFileBrowser();
          break;
        case kMenuItemStats:
          activityManager.pushActivity(std::make_unique<StatsActivity>(renderer, mappedInput));
          break;
        case kMenuItemTransfer:
          activityManager.pushActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
          break;
        case kMenuItemSettings:
          activityManager.goToSettings();
          break;
      }
      break;
  }
}

// ---------- Geometry ----------

Rect HomeActivity::heroRect() const {
  const int pageWidth = renderer.getScreenWidth();
  return Rect{0, kHeroPaddingTop, pageWidth, HomeRenderer::kHeroHeight};
}

Rect HomeActivity::dividerRect() const {
  const int pageWidth = renderer.getScreenWidth();
  return Rect{0, kHeroPaddingTop + HomeRenderer::kHeroHeight + kHeroBottomGap / 2, pageWidth,
              HomeRenderer::kDividerHeight};
}

Rect HomeActivity::sectionLabelRect() const {
  // Span the band *between* the two divider rules (centerY ± 18 px) so the
  // renderer can centre the label vertically within the full available space.
  const int pageWidth = renderer.getScreenWidth();
  const int centerY = kHeroPaddingTop + HomeRenderer::kHeroHeight + kHeroBottomGap / 2;
  constexpr int kHalfBand = 18;  // matches drawDivider's kHalfSeparation
  return Rect{0, centerY - kHalfBand, pageWidth, kHalfBand * 2};
}

Rect HomeActivity::thumbsRow1Rect() const {
  const int pageWidth = renderer.getScreenWidth();
  return Rect{0, kHeroPaddingTop + HomeRenderer::kHeroHeight + kHeroBottomGap, pageWidth,
              HomeRenderer::kThumbnailRowHeight};
}

Rect HomeActivity::thumbsRow2Rect() const {
  const Rect r1 = thumbsRow1Rect();
  return Rect{r1.x, r1.y + r1.height + kRowGap, r1.width, HomeRenderer::kThumbnailRowHeight};
}

Rect HomeActivity::menuRect() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  return Rect{0, pageHeight - HomeRenderer::kBottomMenuHeight - kMenuPaddingBottom, pageWidth,
              HomeRenderer::kBottomMenuHeight};
}

Rect HomeActivity::focusedItemRect() const {
  switch (focusRow) {
    case FOCUS_HERO:
      return HomeRenderer::getHeroCoverRect(heroRect());
    case FOCUS_THUMBS_R1: {
      const int count = rowItemCount(FOCUS_THUMBS_R1);
      return HomeRenderer::getThumbnailRect(thumbsRow1Rect(), focusIndex, count);
    }
    case FOCUS_THUMBS_R2: {
      const int count = rowItemCount(FOCUS_THUMBS_R2);
      return HomeRenderer::getThumbnailRect(thumbsRow2Rect(), focusIndex, count);
    }
    case FOCUS_MENU:
      return HomeRenderer::getMenuTileRect(menuRect(), focusIndex);
  }
  return Rect{0, 0, 0, 0};
}

// ---------- Rendering ----------

void HomeActivity::renderFull() {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, nullptr);

  if (tiles.empty()) {
    HomeRenderer::drawHeroEmpty(renderer, heroRect());
  } else {
    const RecentBook& heroBook = recentBooks[tiles[0].bookIndices[0]];
    const int8_t heroProgress = HomeProgressCache::getInstance().getProgress(heroBook.path);
    HomeRenderer::drawHero(renderer, heroRect(), heroBook, heroProgress);
  }

  if (tiles.size() > 1) {
    HomeRenderer::drawDivider(renderer, dividerRect());
    HomeRenderer::drawSectionLabel(renderer, sectionLabelRect());

    auto buildRow = [&](int startTile) {
      std::vector<HomeRenderer::ThumbTileView> views;
      const int endTile = std::min(static_cast<int>(tiles.size()), startTile + kThumbsPerRow);
      views.reserve(endTile - startTile);
      for (int t = startTile; t < endTile; ++t) {
        const HomeTile& tile = tiles[t];
        if (tile.bookIndices.empty()) continue;
        const int badgeCount =
            tile.displayStackSize > 0 ? tile.displayStackSize : static_cast<int>(tile.bookIndices.size());
        views.push_back({&recentBooks[tile.bookIndices[0]], badgeCount});
      }
      return views;
    };

    HomeRenderer::drawThumbnailRow(renderer, thumbsRow1Rect(), buildRow(1));

    if (tiles.size() > static_cast<size_t>(1 + kThumbsPerRow)) {
      HomeRenderer::drawThumbnailRow(renderer, thumbsRow2Rect(), buildRow(1 + kThumbsPerRow));
    }
  }

  HomeRenderer::drawBottomMenu(renderer, menuRect());
}

void HomeActivity::render(RenderLock&&) {
  // Menu tiles are top-rounded; the focus border should match. Cover and
  // thumbnail focus stay square since the tiles themselves are rectangular.
  const bool roundTopOnly = (focusRow == FOCUS_MENU);

  if (coverBufferStored && restoreCoverBuffer()) {
    HomeRenderer::drawSelectionBorder(renderer, focusedItemRect(), roundTopOnly, roundTopOnly, false, false);
    renderer.displayBuffer();
    return;
  }

  // Single-pass render. onEnter already loaded recents, covers, and
  // progress so the first frame paints a finished home -- no more
  // shell -> covers -> progress refresh stutter on book close.
  renderFull();
  firstRenderDone = true;

  coverBufferStored = storeCoverBuffer();
  if (coverBufferStored) {
    HomeRenderer::drawSelectionBorder(renderer, focusedItemRect(), roundTopOnly, roundTopOnly, false, false);
  }
  renderer.displayBuffer();
}
