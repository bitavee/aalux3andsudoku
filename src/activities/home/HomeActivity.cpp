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
#include "activities/reader/ReaderUtils.h"  // GO_HOME_MS — shared long-press threshold across the UI
#include "activities/stats/StatsActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/HomeProgressCache.h"
#include "components/HomeRenderer.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"

namespace {
constexpr int kThumbsPerRow = 3;
// The home grid caps at 1 hero + 3 thumbs (= 4 tiles), but we no longer cap
// how many *recents* we load: a single tile can be a series stack containing
// many books, and the drill-in viewer needs every member to be present.
// Display tile count is enforced separately at render time.
constexpr int kMaxHomeRecents = 0x7FFFFFFF;  // effectively no cap
constexpr int kHeroPaddingTop = 50;          // hero starts immediately under header
// Section break holds the double-line divider with the "Recent Reads" label
// sandwiched between the two rules. The single thumbnail row sits below with
// breathing room before the menu, and the button-hint band hugs the bottom
// edge. 50 + 300 + 84 + 230 + 26 + 80 + 30 = 800.
constexpr int kHeroBottomGap = 84;
constexpr int kMenuPaddingBottom = 0;  // menu sits directly above the button-hint band
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
    // Long-press Confirm on a single-book tile = "Remove from recents".
    // Series stacks and menu rows fall through to short-press (open),
    // because removing a series-stack member is a separate UX problem.
    if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS && focusedSingleBook() != nullptr) {
      confirmRemoveFocusedBook();
    } else {
      openFocused();
    }
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
    // Per-book Storage.exists() check removed in 1.2.0 — it cost ~5-10ms per book (~50-90ms
    // total on a 9-book recents list) on every home entry just to filter out books the user
    // deleted from the SD card while not on home. The RecentBooks store is authoritative; if
    // a book is gone, file ops downstream (cover load, reader open) fail gracefully into
    // their existing placeholder paths. Worst case: a dead tile sits there until the user
    // selects it, gets a load error, and the entry is cleaned up.

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

  // Pin each stack's cover to the first book in series order so the home
  // tile and the drill-in viewer agree on which member fronts the group.
  // Without this, bookIndices[0] inherits from recents order, which makes
  // the cover whichever member was opened most recently. Skip tile 0 (hero
  // alone, single index) and any tile already sorted (hero series tile).
  for (size_t t = 1; t < tiles.size(); ++t) {
    HomeTile& tile = tiles[t];
    if (tile.bookIndices.size() < 2) continue;
    std::sort(tile.bookIndices.begin(), tile.bookIndices.end(),
              [&](int a, int b) { return orderKey(recentBooks[a]) < orderKey(recentBooks[b]); });
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
    case FOCUS_MENU:
      return kMenuItemCount;
  }
  return 0;
}

void HomeActivity::moveFocus(int dRow, int dCol) {
  if (dRow != 0) {
    constexpr FocusRow rowOrder[] = {FOCUS_HERO, FOCUS_THUMBS_R1, FOCUS_MENU};
    constexpr int rowOrderCount = sizeof(rowOrder) / sizeof(rowOrder[0]);

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

      focusRow = candidate;
      focusIndex = 0;
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
  // Release the 48KB cover-snapshot buffer before launching any sub-activity.
  // The buffer is only useful for fast selection-border redraws while staying
  // on home; once we navigate away it just sits on the stack consuming heap
  // that heap-hungry sub-activities (notably CrossPointWebServerActivity with
  // its WiFi + WebServer + WebSocketsServer + mDNS stack) need to start
  // cleanly. onExit only frees it when home is destroyed, not when pushed,
  // so without this call the buffer survives the push and silently starves
  // the sub-activity. The next home render after pop repopulates the buffer.
  freeCoverBuffer();

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
      openTile(focusIndex + 1);  // hero = tiles[0], R1 = tiles[1..3]
      break;
    case FOCUS_MENU:
      switch (focusIndex) {
        case kMenuItemBrowse:
          activityManager.goToBookshelf();
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

const RecentBook* HomeActivity::focusedSingleBook() const {
  int tileIndex = -1;
  switch (focusRow) {
    case FOCUS_HERO:
      tileIndex = 0;
      break;
    case FOCUS_THUMBS_R1:
      tileIndex = focusIndex + 1;
      break;
    case FOCUS_MENU:
      return nullptr;
  }
  if (tileIndex < 0 || tileIndex >= static_cast<int>(tiles.size())) return nullptr;
  const HomeTile& tile = tiles[tileIndex];
  if (tile.bookIndices.size() != 1) return nullptr;  // skip series stacks
  const int bookIdx = tile.bookIndices[0];
  if (bookIdx < 0 || bookIdx >= static_cast<int>(recentBooks.size())) return nullptr;
  return &recentBooks[bookIdx];
}

void HomeActivity::confirmRemoveFocusedBook() {
  const RecentBook* book = focusedSingleBook();
  if (!book) return;

  // Capture the path now -- we re-resolve via focusedSingleBook() inside the
  // result handler in case the focus moved or the list shifted while the
  // dialog was open. The path is the stable key in recents.
  const std::string targetPath = book->path;
  const std::string title = book->title.empty() ? book->path : book->title;

  // Match the openFocused() behaviour: drop the 48KB cover snapshot before
  // pushing a sub-activity, so we don't starve the dialog's renderer.
  freeCoverBuffer();

  auto handler = [this, targetPath](const ActivityResult& result) {
    if (result.isCancelled) return;
    if (!RECENT_BOOKS.removeBook(targetPath)) {
      LOG_DBG("HOME", "removeBook no-op for %s (already gone?)", targetPath.c_str());
      return;
    }
    LOG_DBG("HOME", "Removed from recents: %s", targetPath.c_str());

    // Rebuild the visible recents list and tiles from the now-shrunk store.
    recentBooks.clear();
    tiles.clear();
    coverBufferStored = false;
    firstRenderDone = false;
    loadRecentBooks(kMaxHomeRecents);

    // Snap focus back somewhere valid: the focused index may now be past the
    // end of its row (or the row may have collapsed entirely).
    if (recentBooks.empty()) {
      focusRow = FOCUS_MENU;
      focusIndex = 0;
    } else {
      const int rowCount = rowItemCount(focusRow);
      if (rowCount <= 0) {
        focusRow = FOCUS_HERO;
        focusIndex = 0;
      } else if (focusIndex >= rowCount) {
        focusIndex = rowCount - 1;
      }
    }
    requestUpdate();
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_RECENTS), title), handler);
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

Rect HomeActivity::menuRect() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int menuY =
      pageHeight - HomeRenderer::kButtonHintsHeight - HomeRenderer::kBottomMenuHeight - kMenuPaddingBottom;
  return Rect{0, menuY, pageWidth, HomeRenderer::kBottomMenuHeight};
}

Rect HomeActivity::focusedItemRect() const {
  switch (focusRow) {
    case FOCUS_HERO:
      return HomeRenderer::getHeroCoverRect(heroRect());
    case FOCUS_THUMBS_R1: {
      const int count = rowItemCount(FOCUS_THUMBS_R1);
      return HomeRenderer::getThumbnailRect(thumbsRow1Rect(), focusIndex, count);
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
  const Rect headerRect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight};
  // Draw header chrome (battery widget) with no title — drawHeader's title
  // path uses UI_12 BOLD at rect.y+5, which is visually heavier than what
  // home wants. Render the brand manually so it shares the battery
  // percentage's baseline (y+5 in header coords; the battery widget itself
  // is rect.y+5 inside drawHeader, and its percentage text uses SMALL_FONT
  // at that rect's y). Bold for prominence.
  GUI.drawHeader(renderer, headerRect, nullptr);
  renderer.drawCenteredText(SMALL_FONT_ID, headerRect.y + 5, tr(STR_AALU), /*black=*/true, EpdFontFamily::BOLD);
  HomeRenderer::drawHeaderClock(renderer, headerRect);

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
  }

  HomeRenderer::drawBottomMenu(renderer, menuRect());

  const Rect menu = menuRect();
  const Rect buttonHintsRect{0, menu.y + menu.height, renderer.getScreenWidth(), HomeRenderer::kButtonHintsHeight};
  HomeRenderer::drawBottomButtonHints(renderer, buttonHintsRect);
}

void HomeActivity::render(RenderLock&&) {
  // Selection overlays: menu rows fill the focused tile with a solid black
  // pill (label rendered in white, no icon) so it matches the crosspet look.
  // Hero and thumbnail rows still get a rectangular focus border since those
  // tiles are larger and a fill would obscure the cover art.
  auto drawFocus = [&]() {
    if (focusRow == FOCUS_MENU) {
      HomeRenderer::drawMenuSelection(renderer, menuRect(), focusIndex);
    } else {
      HomeRenderer::drawSelectionBorder(renderer, focusedItemRect());
    }
  };

  if (coverBufferStored && restoreCoverBuffer()) {
    drawFocus();
    // FAST_REFRESH for selection-border moves — incremental update, no black flash. HALF mode
    // does a full clear-then-redraw cycle that flashes the panel black; FAST mode only writes
    // changed pixels (~50-100ms). Mild ghosting on the selection rectangle is acceptable for
    // arrow-key navigation. First render + re-entry from a sub-activity use FULL_REFRESH.
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // Single-pass render. onEnter already loaded recents, covers, and
  // progress so the first frame paints a finished home -- no more
  // shell -> covers -> progress refresh stutter on book close.
  renderFull();
  firstRenderDone = true;

  // Snapshot the bare home (no selection border) so subsequent focus
  // changes can restore-and-redraw cheaply. This 48 KB malloc can fail
  // when the heap is fragmented — most notably right after a File
  // Transfer / WiFi session, where the WebServer + WebSocket + DNS + mDNS
  // teardown leaves total free heap above 48 KB but no contiguous block.
  coverBufferStored = storeCoverBuffer();
  // ALWAYS draw the selection overlay, regardless of whether the snapshot
  // succeeded. Previously this was gated by `coverBufferStored`, which
  // meant a fragmented-heap malloc failure made the user's focus
  // disappear from the home grid until reboot. The snapshot is just a
  // render-perf optimization for the *next* frame; the overlay itself must
  // show on this frame either way.
  drawFocus();
  renderer.displayBuffer();
}
