#include "HomeActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/stats/StatsActivity.h"
#include "components/HomeProgressCache.h"
#include "components/HomeRenderer.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"

namespace {
constexpr int kThumbsPerRow = 4;
constexpr int kMaxHomeRecents = 1 + 2 * kThumbsPerRow;  // hero + 2 rows of 4
constexpr int kHeroPaddingTop = 50;       // hero starts immediately under header
constexpr int kHeroBottomGap = 12;        // gap between hero and divider line
constexpr int kRowGap = 12;               // gap between two thumbnail rows
constexpr int kMenuPaddingBottom = 20;    // bottom margin under menu tiles
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
  heroProgressLoaded = false;
  freeCoverBuffer();

  loadRecentBooks(kMaxHomeRecents);

  if (recentBooks.empty()) {
    focusRow = FOCUS_MENU;
    focusIndex = 0;
  } else {
    focusRow = FOCUS_HERO;
    focusIndex = 0;
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
  for (const RecentBook& book : books) {
    if (static_cast<int>(recentBooks.size()) >= max) break;
    if (!Storage.exists(book.path.c_str())) continue;
    recentBooks.push_back(book);
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
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
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
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
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
  switch (row) {
    case FOCUS_HERO:
      return !recentBooks.empty();
    case FOCUS_THUMBS_R1:
      return recentBooks.size() > 1;
    case FOCUS_THUMBS_R2:
      return recentBooks.size() > 1 + kThumbsPerRow;  // need a 6th book to populate row 2
    case FOCUS_MENU:
      return true;
  }
  return false;
}

int HomeActivity::rowItemCount(FocusRow row) const {
  switch (row) {
    case FOCUS_HERO:
      return 1;
    case FOCUS_THUMBS_R1:
      return std::min(kThumbsPerRow, std::max(0, static_cast<int>(recentBooks.size()) - 1));
    case FOCUS_THUMBS_R2:
      return std::min(kThumbsPerRow, std::max(0, static_cast<int>(recentBooks.size()) - 1 - kThumbsPerRow));
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
  switch (focusRow) {
    case FOCUS_HERO:
      if (!recentBooks.empty()) {
        activityManager.goToReader(recentBooks[0].path);
      }
      break;
    case FOCUS_THUMBS_R1: {
      const int bookIdx = focusIndex + 1;  // hero is recents[0], R1 = recents[1..4]
      if (bookIdx < static_cast<int>(recentBooks.size())) {
        activityManager.goToReader(recentBooks[bookIdx].path);
      }
      break;
    }
    case FOCUS_THUMBS_R2: {
      const int bookIdx = focusIndex + 1 + kThumbsPerRow;  // R2 = recents[5..8]
      if (bookIdx < static_cast<int>(recentBooks.size())) {
        activityManager.goToReader(recentBooks[bookIdx].path);
      }
      break;
    }
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

  if (recentBooks.empty()) {
    HomeRenderer::drawHeroEmpty(renderer, heroRect());
  } else {
    const int8_t heroProgress = HomeProgressCache::getInstance().getProgress(recentBooks[0].path);
    HomeRenderer::drawHero(renderer, heroRect(), recentBooks[0], heroProgress);
  }

  if (recentBooks.size() > 1) {
    HomeRenderer::drawDivider(renderer, dividerRect());

    const int total = static_cast<int>(recentBooks.size());
    const int row1End = std::min(total, 1 + kThumbsPerRow);
    std::vector<RecentBook> row1(recentBooks.begin() + 1, recentBooks.begin() + row1End);
    HomeRenderer::drawThumbnailRow(renderer, thumbsRow1Rect(), row1);

    if (total > 1 + kThumbsPerRow) {
      std::vector<RecentBook> row2(recentBooks.begin() + 1 + kThumbsPerRow, recentBooks.end());
      HomeRenderer::drawThumbnailRow(renderer, thumbsRow2Rect(), row2);
    }
  }

  HomeRenderer::drawBottomMenu(renderer, menuRect());
}

void HomeActivity::render(RenderLock&&) {
  if (coverBufferStored && restoreCoverBuffer()) {
    HomeRenderer::drawSelectionBorder(renderer, focusedItemRect());
    renderer.displayBuffer();
    return;
  }

  renderFull();

  if (!firstRenderDone) {
    firstRenderDone = true;
    renderer.displayBuffer();
    requestUpdate();
    return;
  }

  if (!recentsLoaded && !recentsLoading) {
    loadRecentCovers();
    renderFull();
  }

  if (recentsLoaded && !heroProgressLoaded && !recentBooks.empty()) {
    HomeProgressCache::getInstance().loadProgressFor(recentBooks[0].path);
    heroProgressLoaded = true;
    renderFull();
  }

  coverBufferStored = storeCoverBuffer();
  if (coverBufferStored) {
    HomeRenderer::drawSelectionBorder(renderer, focusedItemRect());
  }
  renderer.displayBuffer();
}
