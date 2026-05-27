#include "activities/stats/StatsActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "DetailedStatsActivity.h"  // detailedStatistics
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderUtils.h"  // GO_HOME_MS — shared long-press threshold
#include "activities/util/ConfirmationActivity.h"
#include "components/HomeRenderer.h"  // kThumbnailCoverHeight — shared with home/groups so all three pages read the same on-disk thumb
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "stats/ReadingStatsManager.h"

static constexpr int COVER_PAD = 6;

// A book qualifies for the stats list only if the user has actually engaged
// with it. Books at 0% AND with 0 reading time are typically stale entries
// (added to the cache but never opened past the cover, or imported from a
// path that pre-populated metadata). They clutter the list with no useful
// signal.
static bool isStatsVisible(const BookStatEntry& book) { return book.progressPercent > 0 && book.totalReadingMs > 0; }

// -----------------------------------------------------------------------
// Static helpers
// -----------------------------------------------------------------------

/**
 * @brief Formats a duration in milliseconds into a human-readable string (e.g., "Xh Ym" or "Ym").
 */
void StatsActivity::formatDuration(char* buf, size_t bufLen, uint32_t ms) {
  const uint32_t totalMin = ms / 60000UL;
  const uint32_t hours = totalMin / 60UL;
  const uint32_t mins = totalMin % 60UL;
  if (hours > 0) {
    snprintf(buf, bufLen, "%uh %02um", static_cast<unsigned>(hours), static_cast<unsigned>(mins));
  } else {
    snprintf(buf, bufLen, "%um", static_cast<unsigned>(mins));
  }
}

/**
 * @brief Formats a progress percentage into a string (e.g., "100%").
 */
// void StatsActivity::formatPercent(char* buf, size_t bufLen, uint8_t percent) {
//   snprintf(buf, bufLen, "%u%%", static_cast<unsigned>(percent));
// }

/**
 * @brief Returns the number of books that have not yet reached 100% completion.
 * Completed books are filtered out from the active statistics list.
 */
// Counts books in the current view mode (Reading vs Finished). Hidden books
// (0% progress or 0 reading time) are excluded — see isStatsVisible.
uint8_t StatsActivity::getVisibleBookCount() const {
  uint8_t count = 0;
  for (uint8_t i = 0; i < StatsManager.getBookCount(); ++i) {
    const BookStatEntry& book = StatsManager.getBook(i);
    if (!isStatsVisible(book)) continue;
    const bool isDone = (book.progressPercent >= 95);
    if (isDone == showingFinished) count++;
  }
  return count;
}

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

void StatsActivity::onEnter() {
  Activity::onEnter();
  selectedBookIndex = 0;
  // Stats can outlive thumb files (Clear Cache, file moves, books opened on a
  // firmware version that didn't pre-generate thumbs). Regenerate any missing
  // covers up front so every row has artwork to render.
  prepareMissingCovers();
  activityManager.requestUpdateAndWait();
}

void StatsActivity::prepareMissingCovers() {
  // Stats reads thumbnails at the same resolution the home and group pages
  // use (kThumbnailCoverHeight), so the on-disk cache file is shared across
  // pages — no duplicate regen, and any cover that renders on home will
  // render here. Targeting one canonical size also means the renderer can
  // call drawBitmapStretched1Bit with a known source aspect, fixing the old
  // white-margin bug that came from drawBitmap's aspect-fit fallback.
  static constexpr int kRegenHeight = HomeRenderer::kThumbnailCoverHeight;

  const uint8_t total = StatsManager.getBookCount();
  if (total == 0) return;

  bool popupShown = false;
  Rect popupRect;

  for (uint8_t i = 0; i < total; ++i) {
    const BookStatEntry& book = StatsManager.getBook(i);
    if (!isStatsVisible(book)) continue;
    if (book.thumbBmpPath[0] == '\0') continue;
    if (book.bookPath[0] == '\0') continue;

    const std::string thumbPath = UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), kRegenHeight);
    if (Storage.exists(thumbPath.c_str())) continue;

    // The book file itself may have moved/been deleted - skip silently.
    if (!Storage.exists(book.bookPath)) continue;

    // Only EPUB regen is wired up right now. TXT/XTC entries fall through to
    // the placeholder in the row renderer (matches home/group behaviour).
    if (!FsHelpers::hasEpubExtension(std::string(book.bookPath))) continue;

    if (!popupShown) {
      popupShown = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    }
    GUI.fillPopupProgress(renderer, popupRect, 10 + (i * 90) / std::max<uint8_t>(total, 1));

    Epub epub(std::string(book.bookPath), "/.crosspoint");
    if (!epub.load(false, true)) {
      LOG_DBG("STATS", "Could not load EPUB for cover regen: %s", book.bookPath);
      continue;
    }
    if (!epub.generateThumbBmp(kRegenHeight)) {
      LOG_DBG("STATS", "Cover regen failed (no embedded image?) for %s", book.bookPath);
    }
  }
}

void StatsActivity::onExit() { Activity::onExit(); }

// -----------------------------------------------------------------------
// Input Handling
// -----------------------------------------------------------------------

void StatsActivity::loop() {
  // Return to the Home screen
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.popActivity();
    return;
  }

  // Toggle between Reading and Finished lists. Must be handled BEFORE the
  // empty-list early return — otherwise an empty current list traps the user
  // (e.g. when Finished has no entries, Right would no-op and the only way
  // back to Reading is Back -> re-enter Stats).
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    showingFinished = !showingFinished;
    selectedBookIndex = 0;
    requestUpdate();
    return;
  }

  const uint8_t bookCount = getVisibleBookCount();
  if (bookCount == 0) return;

  bool changed = false;

  // Navigate up in the book list
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (selectedBookIndex > 0) {
      selectedBookIndex--;
      changed = true;
    }
  }

  // Navigate down in the book list
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (selectedBookIndex < static_cast<int>(bookCount) - 1) {
      selectedBookIndex++;
      changed = true;
    }
  }

  const uint8_t actualMemoryIndex = resolveSelectedMemoryIndex();

  // Open detailed stats (More...)
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    activityManager.pushActivity(std::make_unique<DetailedStatsActivity>(renderer, mappedInput, actualMemoryIndex));
    return;
  }

  // Confirm: short press opens the book, long press prompts to remove it from
  // the stats list. Mirrors the home-screen recents long-press gesture.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
      confirmRemoveFocusedBook();
      return;
    }
    const BookStatEntry& book = StatsManager.getBook(actualMemoryIndex);
    if (book.bookPath[0] != '\0') {
      activityManager.goToReader(std::string(book.bookPath));
    }
    return;
  }

  if (changed) {
    requestUpdate();
  }
}

uint8_t StatsActivity::resolveSelectedMemoryIndex() const {
  int currentMatch = 0;
  for (uint8_t j = 0; j < StatsManager.getBookCount(); ++j) {
    const BookStatEntry& book = StatsManager.getBook(j);
    if (!isStatsVisible(book)) continue;
    const bool isDone = (book.progressPercent >= 95);
    if (isDone != showingFinished) continue;
    if (currentMatch == selectedBookIndex) return j;
    currentMatch++;
  }
  return 0xFF;
}

void StatsActivity::confirmRemoveFocusedBook() {
  const uint8_t memoryIndex = resolveSelectedMemoryIndex();
  if (memoryIndex == 0xFF) return;

  // Snapshot the cacheKey now -- the dialog runs as a sub-activity, so the
  // underlying array could in theory be mutated before the handler fires
  // (e.g. a background end-of-session save). Re-resolving via cacheKey on
  // confirm keeps us pointed at the right row even if indices have shifted.
  char cacheKey[sizeof(BookStatEntry::cacheKey)];
  strncpy(cacheKey, StatsManager.getBook(memoryIndex).cacheKey, sizeof(cacheKey));
  cacheKey[sizeof(cacheKey) - 1] = '\0';

  const std::string title = StatsManager.getBook(memoryIndex).title;

  auto handler = [this, cacheKey = std::string(cacheKey)](const ActivityResult& result) {
    if (result.isCancelled) return;

    uint8_t target = 0xFF;
    for (uint8_t i = 0; i < StatsManager.getBookCount(); ++i) {
      if (strncmp(StatsManager.getBook(i).cacheKey, cacheKey.c_str(), sizeof(BookStatEntry::cacheKey)) == 0) {
        target = i;
        break;
      }
    }
    if (target == 0xFF) return;
    if (!StatsManager.removeBook(target)) return;

    const uint8_t remaining = getVisibleBookCount();
    if (remaining == 0) {
      selectedBookIndex = 0;
    } else if (selectedBookIndex >= static_cast<int>(remaining)) {
      selectedBookIndex = static_cast<int>(remaining) - 1;
    }
    requestUpdate();
  };

  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_REMOVE_FROM_STATS), title), handler);
}

// -----------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------

void StatsActivity::render(RenderLock&& lock) {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  renderer.clearScreen();

  // Draw standard header
  GUI.drawHeader(renderer, Rect(0, metrics.topPadding, screenW, metrics.headerHeight), tr(STR_STATS_TITLE));

  const int contentTop = metrics.topPadding + metrics.headerHeight;
  const int contentH = screenH - contentTop - metrics.buttonHintsHeight;

  // Layout split: Top 25% for global stats, bottom 75% for book list
  const int topH = contentH / 4;
  const int bottomH = contentH - topH;

  renderTopPanel(contentTop, topH, screenW);
  renderBookPanel(contentTop + topH, bottomH, screenW);

  GUI.drawButtonHintsGlyphs(renderer, BaseTheme::ButtonHintGlyphSet::StatsActions);

  renderer.displayBuffer();
}

// -----------------------------------------------------------------------
// Top Panel — Global Statistics (All Time & Last 7 Sessions)
// -----------------------------------------------------------------------
void StatsActivity::renderTopPanel(int panelY, int panelH, int screenW) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto& global = StatsManager.getGlobal();
  const int pad = metrics.contentSidePadding;

  // Bottom divider and vertical column separator
  renderer.drawLine(0, panelY + panelH, screenW, panelY + panelH, 1, true);
  renderer.drawLine(screenW / 2, panelY + 8, screenW / 2, panelY + panelH - 8, 1, true);

  const int col1X = pad;
  const int col2X = screenW / 2 + pad;

  // Evenly space 3 rows: headers, time values, session counts
  const int rowStep = panelH / 4;
  const int row1Y = panelY + rowStep / 2;  // Column headers
  const int row2Y = row1Y + rowStep;       // Time values
  // const int row3Y = row2Y + rowStep;       // Session values

  // Column 1: Header - All Time Stats
  renderer.drawText(UI_12_FONT_ID, col1X, row1Y, tr(STR_STATS_ALL_TIME), true, EpdFontFamily::BOLD);

  // Column 2: Header - Global Milestone
  renderer.drawText(UI_12_FONT_ID, col2X, row1Y, tr(STR_FINISHED_BOOKS), true, EpdFontFamily::BOLD);

  // Value: Total cumulative reading hours
  char bufAllTime[16];
  formatDuration(bufAllTime, sizeof(bufAllTime), global.totalReadingMs);
  renderer.drawText(UI_12_FONT_ID, col1X, row2Y, bufAllTime, true);

  // Value: Finished Books count
  char bufFinished[12];
  snprintf(bufFinished, sizeof(bufFinished), "%u", static_cast<unsigned>(global.totalBooksFinished));
  renderer.drawText(UI_12_FONT_ID, col2X, row2Y, bufFinished, true);
}

// -----------------------------------------------------------------------
// Bottom Panel — Scrollable Book List
// -----------------------------------------------------------------------
void StatsActivity::renderBookPanel(int panelY, int panelH, int screenW) const {
  const uint8_t count = getVisibleBookCount();

  if (count == 0) {
    renderer.drawCenteredText(UI_12_FONT_ID, panelY + panelH / 2, tr(STR_STATS_NO_DATA), true);
    return;
  }

  static constexpr int VISIBLE_ROWS = 3;
  const int rowH = panelH / VISIBLE_ROWS;
  const int scrollOffset = (selectedBookIndex / VISIBLE_ROWS) * VISIBLE_ROWS;
  const int visibleCount = std::min(static_cast<int>(count) - scrollOffset, VISIBLE_ROWS);

  for (int i = 0; i < visibleCount; ++i) {
    const int visibleIdx = scrollOffset + i;
    const int rowY = panelY + i * rowH;

    const BookStatEntry* targetBook = nullptr;
    int currentMatch = 0;

    for (uint8_t j = 0; j < StatsManager.getBookCount(); ++j) {
      const BookStatEntry& book = StatsManager.getBook(j);
      if (!isStatsVisible(book)) continue;
      const bool isDone = (book.progressPercent >= 95);
      if (isDone != showingFinished) continue;

      if (currentMatch == visibleIdx) {
        targetBook = &book;
        break;
      }
      currentMatch++;
    }

    if (targetBook) {
      renderBookRow(0, rowY, screenW, rowH, *targetBook, visibleIdx == selectedBookIndex);
    }
  }
}

// -----------------------------------------------------------------------
// Single Book Row Rendering
// -----------------------------------------------------------------------
// src/activities/stats/StatsActivity.cpp

void StatsActivity::renderBookRow(int rowX, int rowY, int rowW, int rowH, const BookStatEntry& book,
                                  bool selected) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pad = metrics.contentSidePadding;

  // Top divider for the row
  renderer.drawLine(rowX, rowY, rowX + rowW, rowY, 1, true);

  // Visual highlight for the currently selected book (dithered background)
  if (selected) {
    renderer.fillRectDither(rowX, rowY + 1, rowW, rowH - 1, LightGray);
    renderer.drawRect(rowX, rowY + 1, rowW, rowH - 1, 2, true);
  }

  // Cover dimensions: Portrait rectangle (3:4 aspect ratio to match physical books)
  const int coverH = rowH - COVER_PAD * 2;
  const int coverW = (coverH * 3) / 4;
  const int coverX = rowX + COVER_PAD;
  const int coverY = rowY + COVER_PAD;

  // Attempt to draw the generated cover; fallback to a title placeholder if
  // the book has no thumbnail at all (no embedded cover image, generation
  // failed, or the cache was cleared).
  if (!loadAndDrawCover(coverX, coverY, coverW, coverH, book)) {
    drawCoverPlaceholder(coverX, coverY, coverW, coverH, book.title);
  }

  const int textX = coverX + coverW + pad + 10;
  const int textW = rowX + rowW - textX - pad - 10;
  const int line1Y = coverY + 10;

  // --- FIX: Proper Title Truncation Logic ---
  char truncatedTitle[64];
  size_t titleLen = strlen(book.title);
  // Using a strict 16 character limit to prevent overlap with the percentage text
  static constexpr size_t MAX_LIST_TITLE_LEN = 26;

  if (titleLen > MAX_LIST_TITLE_LEN) {
    strncpy(truncatedTitle, book.title, MAX_LIST_TITLE_LEN);
    truncatedTitle[MAX_LIST_TITLE_LEN] = '\0';
    strcat(truncatedTitle, "...");
  } else {
    strncpy(truncatedTitle, book.title, sizeof(truncatedTitle) - 1);
  }

  // Line 1 — Book Title (Bold, UI_12) - Using the truncated buffer!
  renderer.drawText(UI_12_FONT_ID, textX, line1Y, truncatedTitle, true, EpdFontFamily::BOLD);

  // Distribute text lines evenly across the usable row height
  const int line2Y = coverY + 40;  // Progress bar
  const int line3Y = coverY + 70;  // Time spent
  const int line4Y = coverY + 95;  // Sessions

  // Line 2 — Custom Progress Bar + Percentage Text (UI_10)
  const int barH = 8;
  const int barY = line2Y + 4;
  const int pctLabelW = 40;                // Reserved width for "100%"
  const int barW = textW - pctLabelW - 5;  // Prevents overlap with text
  const int pctX = textX + barW + 10;

  if (barW > 10) {
    HomeRenderer::drawRoundedProgressBar(renderer, textX, barY, barW, barH, static_cast<int8_t>(book.progressPercent));
    char bufPct[6];
    snprintf(bufPct, sizeof(bufPct), "%u%%", static_cast<unsigned>(book.progressPercent));
    // Vertically centre the label on the bar (same trick as the home hero) so
    // the percent text doesn't float above the rounded ends.
    const int pctY = barY + (barH - renderer.getLineHeight(UI_10_FONT_ID)) / 2 - 1;
    renderer.drawText(UI_10_FONT_ID, pctX, pctY, bufPct, true);
  }

  // Line 3 — Time Spent (UI_10)
  char bufDur[16];
  char bufLine3[40];
  formatDuration(bufDur, sizeof(bufDur), book.totalReadingMs);
  snprintf(bufLine3, sizeof(bufLine3), "%s: %s", tr(STR_STATS_TIME_SPENT), bufDur);
  renderer.drawText(UI_10_FONT_ID, textX, line3Y, bufLine3, true);

  // Line 4 — Session Count (UI_10)
  char bufLine4[32];
  snprintf(bufLine4, sizeof(bufLine4), "%s: %u", tr(STR_STATS_SESSIONS_COUNT),
           static_cast<unsigned>(book.sessionCount));
  renderer.drawText(UI_10_FONT_ID, textX, line4Y, bufLine4, true);
}

// -----------------------------------------------------------------------
// Cover Drawing and Placeholders
// -----------------------------------------------------------------------

/**
 * @brief Draws a fallback placeholder when a book cover is missing.
 */
void StatsActivity::drawCoverPlaceholder(int x, int y, int w, int h, const char* /*title*/) const {
  // Empty bordered rect - matches the home screen's behaviour for books with
  // no embedded cover image. The book title is already visible to the right
  // of the cover in the row layout, so we don't paint it inside the box.
  renderer.drawRoundedRect(x, y, w, h, 1, 4, false);
}

/**
 * @brief Loads and renders a book cover, mirroring HomeRenderer::drawCover.
 *
 * Uses the same single canonical resolution (kThumbnailCoverHeight) and the
 * same is1Bit() dispatch as the home page and series-group page, so all three
 * surfaces read the same on-disk thumb and render it identically. Falls back
 * to drawCoverPlaceholder on any miss; never renders a degraded cover.
 *
 * @return true if a valid cover was found and successfully rendered.
 */
bool StatsActivity::loadAndDrawCover(int x, int y, int w, int h, const BookStatEntry& book) const {
  if (book.thumbBmpPath[0] == '\0') {
    return false;
  }

  const std::string thumbPath =
      UITheme::getCoverThumbPath(std::string(book.thumbBmpPath), HomeRenderer::kThumbnailCoverHeight);

  FsFile f;
  if (!Storage.openFileForRead("STATS", thumbPath.c_str(), f)) return false;

  Bitmap bmp(f, false);
  if (bmp.parseHeaders() != BmpReaderError::Ok) {
    f.close();
    return false;
  }

  // 1-bit thumbs are stretched non-uniformly to fill the slot exactly, so the
  // cover never leaves a white margin on the right when its source aspect
  // ratio doesn't match the row's 3:4 cover slot. Identical dispatch to
  // HomeRenderer::drawCover.
  if (bmp.is1Bit()) {
    renderer.drawBitmapStretched1Bit(bmp, x, y, w, h);
  } else {
    renderer.drawBitmap(bmp, x, y, w, h);
  }
  renderer.drawRect(x, y, w, h);

  f.close();
  return true;
}