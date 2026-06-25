#include "BootActivity.h"

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
#include "components/HomeRenderer.h"  // drawRoundedProgressBar — shared with home hero and stats list for a consistent indicator
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "stats/ReadingStatsManager.h"

namespace {

// Cover height the boot resuming card requests from the thumb cache. Bound to
// the recents-row thumbnail height so any book that has been opened before
// already has the BMP on disk -- no thumb generation during boot. These two
// MUST stay equal: the thumb filename embeds the height (thumb_<H>.bmp), and
// only kThumbnailCoverHeight is ever generated, so a divergent value would
// always miss on disk and render a blank placeholder.
constexpr int kBootCoverThumbHeight = HomeRenderer::kThumbnailCoverHeight;

constexpr int kBootCardCoverW = 120;
constexpr int kBootCardCoverH = 180;
constexpr int kBootCardGap = 20;
constexpr int kBootCardMetaW = 260;
constexpr int kBootCardW = kBootCardCoverW + kBootCardGap + kBootCardMetaW;

// Approximate human-friendly duration ("6h", "1h 12m", "47m", "<1m"). Mirrors
// the formatter in HomeRenderer so the boot card and the home hero report
// time-left in the same shape.
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

// Draws the cover at (x, y, w, h). Falls back to a bordered placeholder when
// the thumb cannot be decoded -- the resume card is still useful without art.
void drawCover(GfxRenderer& renderer, int x, int y, int w, int h, const RecentBook& book) {
  if (book.coverBmpPath.empty()) {
    renderer.roundCoverCorners(x, y, w, h, HomeRenderer::kCoverCornerRadius);
    return;
  }
  const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, kBootCoverThumbHeight);
  FsFile file;
  if (!Storage.openFileForRead("BOOT", thumbPath, file)) {
    renderer.roundCoverCorners(x, y, w, h, HomeRenderer::kCoverCornerRadius);
    return;
  }
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    renderer.roundCoverCorners(x, y, w, h, HomeRenderer::kCoverCornerRadius);
    return;
  }
  if (bitmap.is1Bit()) {
    renderer.drawBitmapStretched1Bit(bitmap, x, y, w, h);
  } else {
    renderer.drawBitmap(bitmap, x, y, w, h);
  }
  renderer.roundCoverCorners(x, y, w, h, HomeRenderer::kCoverCornerRadius);
  file.close();
}

}  // namespace

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, pageHeight / 2 + 60, tr(STR_AALU), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 106, tr(STR_BOOTING));
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, AALU_VERSION);
  renderer.displayBuffer();
}

void BootActivity::renderResumingCard(GfxRenderer& renderer, const RecentBook& book, int8_t progressPercent) {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();

  renderer.clearScreen();

  // AALU brand mark, pinned to the top. Uses the same font/weight as the
  // boot-logo screen ("AALU" in NOTOSANS_18 BOLD) so device identity reads
  // consistently whether boot lands on the logo screen or this resume card.
  constexpr int kBrandTopMargin = 60;
  const int brandY = kBrandTopMargin;
  renderer.drawCenteredText(NOTOSANS_18_FONT_ID, brandY, tr(STR_AALU), /*black=*/true, EpdFontFamily::BOLD);

  // Card (cover + meta) sits vertically centered in the screen so the
  // composition reads as "AALU header up top, book front-and-center below".
  // "Resuming…" label sits a fixed gap beneath the card.
  const int cardX = (screenW - kBootCardW) / 2;
  const int cardY = (screenH - kBootCardCoverH) / 2;

  // Cover (left).
  drawCover(renderer, cardX, cardY, kBootCardCoverW, kBootCardCoverH, book);

  // Metadata column (right of cover).
  const int metaX = cardX + kBootCardCoverW + kBootCardGap;
  const int metaW = kBootCardMetaW;
  int textY = cardY;

  // Title -- BOOKERLY_14 BOLD, up to 2 wrapped lines. Deliberately smaller
  // than the AALU brand mark above (NOTOSANS_18) so the device identity
  // reads as the page header and the title sits one level down in the
  // hierarchy. Bookerly (the book-content typeface) for the title keeps
  // the typographic vocabulary aligned with the reader.
  const int titleLineH = renderer.getLineHeight(BOOKERLY_14_FONT_ID);
  if (!book.title.empty()) {
    const std::vector<std::string> titleLines =
        renderer.wrappedText(BOOKERLY_14_FONT_ID, book.title.c_str(), metaW, /*maxLines=*/2, EpdFontFamily::BOLD);
    for (const auto& line : titleLines) {
      renderer.drawText(BOOKERLY_14_FONT_ID, metaX, textY, line.c_str(), /*black=*/true, EpdFontFamily::BOLD);
      textY += titleLineH + 2;
    }
    textY += 6;
  }

  // Author.
  const int smallLineH = renderer.getLineHeight(UI_10_FONT_ID);
  if (!book.author.empty()) {
    const std::string author = renderer.truncatedText(UI_10_FONT_ID, book.author.c_str(), metaW);
    renderer.drawText(UI_10_FONT_ID, metaX, textY, author.c_str());
    textY += smallLineH + 4;
  }

  // Series (italic), with "Book N" on its own line when present. Matches the
  // home hero treatment so the boot card carries the same series affordance.
  if (!book.seriesName.empty()) {
    const std::string seriesLine =
        renderer.truncatedText(UI_10_FONT_ID, book.seriesName.c_str(), metaW, EpdFontFamily::ITALIC);
    renderer.drawText(UI_10_FONT_ID, metaX, textY, seriesLine.c_str(), /*black=*/true, EpdFontFamily::ITALIC);
    textY += smallLineH + 2;

    if (!book.seriesIndex.empty()) {
      char bookBuf[32];
      std::snprintf(bookBuf, sizeof(bookBuf), "Book %s", book.seriesIndex.c_str());
      const std::string bookLine = renderer.truncatedText(UI_10_FONT_ID, bookBuf, metaW, EpdFontFamily::ITALIC);
      renderer.drawText(UI_10_FONT_ID, metaX, textY, bookLine.c_str(), /*black=*/true, EpdFontFamily::ITALIC);
      textY += smallLineH + 8;
    } else {
      textY += 6;
    }
  } else {
    textY += 6;
  }

  // Progress bar + percent. Same primitives as the home hero so users see a
  // consistent indicator across boot, home, and reader status bar.
  const int cardBottom = cardY + kBootCardCoverH;
  if (progressPercent >= 0 && textY + 10 <= cardBottom) {
    constexpr int kBarHeight = 10;
    constexpr int kLabelGap = 8;
    const int clampedPct = (progressPercent > 100) ? 100 : static_cast<int>(progressPercent);
    char percentStr[8];
    std::snprintf(percentStr, sizeof(percentStr), "%d%%", clampedPct);
    const int labelW = renderer.getTextWidth(UI_10_FONT_ID, percentStr, EpdFontFamily::BOLD);
    const int barW = metaW - labelW - kLabelGap;
    if (barW > 0) {
      HomeRenderer::drawRoundedProgressBar(renderer, metaX, textY, barW, kBarHeight, static_cast<int8_t>(clampedPct));
      const int labelTextY = textY + (kBarHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2 - 1;
      renderer.drawText(UI_10_FONT_ID, metaX + barW + kLabelGap, labelTextY, percentStr, /*black=*/true,
                        EpdFontFamily::BOLD);
    }
    textY += kBarHeight + 8;

    // Time-left line. Only drawn when (a) per-book stats exist with real
    // reading time, and (b) the book isn't finished -- otherwise "~0h left"
    // is noise. Pace is per-book (totalReadingMs scaled by remaining %) so a
    // slow technical re-read reports more remaining time than a sprint.
    if (clampedPct > 0 && clampedPct < 100 && textY + smallLineH <= cardBottom) {
      const BookStatEntry* stat = findStatsByPath(book.path);
      if (stat && stat->totalReadingMs > 0) {
        const uint32_t remainingPct = 100u - static_cast<uint32_t>(clampedPct);
        const uint64_t remainingMs =
            static_cast<uint64_t>(stat->totalReadingMs) * remainingPct / static_cast<uint32_t>(clampedPct);
        char leftBuf[32];
        formatTimeApprox(leftBuf, sizeof(leftBuf), remainingMs);
        char leftLine[48];
        std::snprintf(leftLine, sizeof(leftLine), "~%s left", leftBuf);
        const std::string leftTrunc = renderer.truncatedText(UI_10_FONT_ID, leftLine, metaW);
        renderer.drawText(UI_10_FONT_ID, metaX, textY, leftTrunc.c_str());
      }
    }
  }

  // "Resuming…" label, centered below the card.
  const int resumingY = cardY + kBootCardCoverH + 20;
  renderer.drawCenteredText(UI_12_FONT_ID, resumingY, tr(STR_RESUMING), /*black=*/true, EpdFontFamily::BOLD);

  renderer.displayBuffer();
}
