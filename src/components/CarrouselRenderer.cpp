#include "CarrouselRenderer.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "CrossPointSettings.h"
#include "HomeRenderer.h"
#include "RecentBooksStore.h"
#include "activities/stats/CatSprites.h"
#include "components/HomeProgressCache.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "stats/ReadingStatsManager.h"

namespace {

constexpr int kTopGap = 8;
constexpr int kInfoGapTop = 6;
constexpr int kInfoGapBottom = 10;
constexpr int kCardsMinHeight = 88;
constexpr int kCardsMaxHeight = 200;
constexpr int kBottomGap = 10;
constexpr int kCoverWidthCapPct = 62;
constexpr int kCardGap = 8;
constexpr int kCardSidePadding = 14;
constexpr int kCardCornerRadius = 6;
constexpr int kCardInnerPad = 6;
constexpr int kSelectionPadding = 4;
constexpr int kSelectionGap = 2;

constexpr int kSideWidthPctOfCenter = 30;
constexpr int kSideInnerPct = 90;
constexpr int kSideOuterPct = 80;
constexpr int kSideOverlapPct = 15;
constexpr int kSideFarStepPct = 75;
constexpr int kMinCoverHeight = 60;

int infoBandHeight(GfxRenderer& renderer) {
  return renderer.getLineHeight(UI_12_FONT_ID) + 2 + renderer.getLineHeight(UI_10_FONT_ID) + 4;
}

bool cardsShown(GfxRenderer& renderer, const Rect& area) {
  if (renderer.getScreenHeight() <= renderer.getScreenWidth()) return false;
  const int need = kTopGap + 200 + kInfoGapTop + infoBandHeight(renderer) + kInfoGapBottom + kCardsMinHeight;
  return area.height >= need;
}

struct CarrouselLayout {
  Rect cover;
  int infoTop;
  Rect cards;
  bool hasCards;
};

CarrouselLayout computeLayout(GfxRenderer& renderer, const Rect& area) {
  const int pageWidth = renderer.getScreenWidth();
  const int cx = pageWidth / 2;

  [[maybe_unused]] int marginTop, marginBottom;
  int marginRight, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  const int viewLeft = marginLeft;
  const int viewRight = pageWidth - marginRight;
  const int halfRoom = std::min(cx - viewLeft, viewRight - cx);
  const int farSafeWidth = halfRoom * 100 / 84;
  const int widthCap = std::min(kCoverWidthCapPct * pageWidth / 100, farSafeWidth);

  const int info = infoBandHeight(renderer);
  const bool hasCards = cardsShown(renderer, area);
  const int coverTop = area.y + kTopGap;
  const int areaBottom = area.y + area.height;

  int reserveBelow = kInfoGapTop + info + kBottomGap;
  if (hasCards) reserveBelow += kInfoGapBottom + kCardsMinHeight;

  int coverH = std::min(area.height - kTopGap - reserveBelow, widthCap * 3 / 2);
  if (coverH < kMinCoverHeight) coverH = kMinCoverHeight;
  int coverW = (coverH * 2) / 3;
  if (coverW > widthCap) {
    coverW = widthCap;
    coverH = (coverW * 3) / 2;
  }

  CarrouselLayout layout;
  layout.cover = Rect{cx - coverW / 2, coverTop, coverW, coverH};
  layout.infoTop = coverTop + coverH + kInfoGapTop;
  layout.hasCards = hasCards;
  if (hasCards) {
    const int cardsTop = layout.infoTop + info + kInfoGapBottom;
    const int cardsH = std::clamp(areaBottom - kBottomGap - cardsTop, kCardsMinHeight, kCardsMaxHeight);
    layout.cards = Rect{area.x, cardsTop, area.width, cardsH};
  } else {
    layout.cards = Rect{area.x, layout.infoTop + info, area.width, 0};
  }
  return layout;
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

struct PetVisual {
  StrId nameKey;
  const CatSprite* sprite;
};

constexpr PetVisual kPetVisuals[stats::kPetStageCount] = {
    {StrId::STR_PET_CAT_KITTEN, &kCatKitten},       {StrId::STR_PET_CAT_ADOLESCENT, &kCatTeen},
    {StrId::STR_PET_CAT_ADULT, &kCatAdult},         {StrId::STR_PET_TIGER_CUB, &kTigerCub},
    {StrId::STR_PET_TIGER_ADOLESCENT, &kTigerTeen}, {StrId::STR_PET_TIGER_ADULT, &kTigerAdult},
    {StrId::STR_PET_DRAGON_EGG, &kDragonEgg},       {StrId::STR_PET_DRAGON_HATCHLING, &kDragonHatch},
    {StrId::STR_PET_DRAGON_JUVENILE, &kDragonJuv},  {StrId::STR_PET_DRAGON_ADULT, &kDragonAdult},
    {StrId::STR_PET_DRAGON_ELDER, &kDragonElder}};

void drawPetSpriteScaled(GfxRenderer& renderer, const CatSprite& s, int boxX, int boxY, int box) {
  const int srcMax = std::max<int>(s.w, s.h);
  if (srcMax <= 0) return;
  const int scaleQ = (box * 256) / srcMax;
  if (scaleQ <= 0) return;
  const int dstW = (static_cast<int>(s.w) * scaleQ) / 256;
  const int dstH = (static_cast<int>(s.h) * scaleQ) / 256;
  const int offX = boxX + (box - dstW) / 2;
  const int offY = boxY + (box - dstH) / 2;
  for (int dy = 0; dy < dstH; ++dy) {
    const int sy = (dy * 256) / scaleQ;
    for (int dx = 0; dx < dstW; ++dx) {
      const int sx = (dx * 256) / scaleQ;
      const int i = sy * static_cast<int>(s.w) + sx;
      const uint8_t v = (s.data[i >> 2] >> ((i & 3) * 2)) & 0x3;
      if (v == 0) continue;
      if (v == 1) {
        renderer.drawPixel(offX + dx, offY + dy, true);
      } else {
        renderer.fillRectDither(offX + dx, offY + dy, 1, 1, LightGray);
      }
    }
  }
}

}  // namespace

CarrouselRenderer::~CarrouselRenderer() { freeTiles(); }

void CarrouselRenderer::reset() { freeTiles(); }

void CarrouselRenderer::freeTiles() {
  for (SideTile& t : tiles) {
    if (t.packed) {
      free(t.packed);
      t.packed = nullptr;
    }
    t.bytes = 0;
    t.width = 0;
    t.height = 0;
    t.bookPath.clear();
  }
}

Rect CarrouselRenderer::centerCoverRect(GfxRenderer& renderer, const Rect& area) {
  return computeLayout(renderer, area).cover;
}

void CarrouselRenderer::drawCoverInto(GfxRenderer& renderer, const Rect& dst, const RecentBook& book,
                                      int targetHeight) {
  const bool bwPass = renderer.getRenderMode() == GfxRenderer::BW;
  if (book.coverBmpPath.empty()) {
    if (bwPass) {
      renderer.roundCoverCorners(dst.x, dst.y, dst.width, dst.height, HomeRenderer::kCoverCornerRadius);
      renderer.drawRoundedRect(dst.x, dst.y, dst.width, dst.height, 1, HomeRenderer::kCoverCornerRadius, true);
    }
    return;
  }
  const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, targetHeight);
  FsFile file;
  bool drawn = false;
  if (Storage.openFileForRead("CARR", thumbPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderer.drawPerspectiveBitmap(bitmap, dst.x, dst.y, dst.width, dst.height, dst.height);
      drawn = true;
    }
    file.close();
  }
  if (bwPass) {
    if (!drawn) {
      renderer.drawRoundedRect(dst.x, dst.y, dst.width, dst.height, 1, HomeRenderer::kCoverCornerRadius, true);
    }
    renderer.roundCoverCorners(dst.x, dst.y, dst.width, dst.height, HomeRenderer::kCoverCornerRadius);
  }
}

void CarrouselRenderer::drawSideTileCached(GfxRenderer& renderer, int slot, const Rect& bbox, const RecentBook& book,
                                           int hL, int hR, int targetHeight) {
  if (slot < 0 || slot >= kSideSlots) return;
  SideTile& tile = tiles[slot];
  const size_t needed = renderer.getRegionByteSize(bbox.x, bbox.y, bbox.width, bbox.height);

  if (tile.packed && tile.bookPath == book.path && tile.width == bbox.width && tile.height == bbox.height &&
      tile.bytes == needed) {
    if (renderer.copyBufferToRegion(bbox.x, bbox.y, bbox.width, bbox.height, tile.packed, tile.bytes)) {
      return;
    }
  }

  renderer.fillRect(bbox.x, bbox.y, bbox.width, bbox.height, false);
  drawPerspectiveCoverInto(renderer, bbox, book, hL, hR, targetHeight);

  if (needed == 0) return;
  if (tile.packed && tile.bytes != needed) {
    free(tile.packed);
    tile.packed = nullptr;
  }
  if (!tile.packed) {
    tile.packed = static_cast<uint8_t*>(malloc(needed));
    if (!tile.packed) {
      LOG_ERR("CARR", "malloc failed for side tile: %u bytes", static_cast<unsigned>(needed));
      tile.bytes = 0;
      tile.bookPath.clear();
      return;
    }
  }
  tile.bytes = needed;
  tile.width = bbox.width;
  tile.height = bbox.height;
  tile.bookPath = book.path;
  if (!renderer.copyRegionToBuffer(bbox.x, bbox.y, bbox.width, bbox.height, tile.packed, tile.bytes)) {
    free(tile.packed);
    tile.packed = nullptr;
    tile.bytes = 0;
    tile.bookPath.clear();
  }
}

void CarrouselRenderer::drawPerspectiveCoverInto(GfxRenderer& renderer, const Rect& bbox, const RecentBook& book,
                                                 int hL, int hR, int targetHeight) {
  const bool bwPass = renderer.getRenderMode() == GfxRenderer::BW;
  if (book.coverBmpPath.empty()) {
    if (bwPass) drawPerspectiveOutline(renderer, bbox, hL, hR, true);
    return;
  }
  const std::string thumbPath = UITheme::getCoverThumbPath(book.coverBmpPath, targetHeight);
  FsFile file;
  bool drawn = false;
  if (Storage.openFileForRead("CARR", thumbPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderer.drawPerspectiveBitmap(bitmap, bbox.x, bbox.y, bbox.width, hL, hR);
      drawn = true;
    }
    file.close();
  }
  if (bwPass) {
    drawPerspectiveOutline(renderer, bbox, hL, hR, !drawn);
  }
}

void CarrouselRenderer::drawPerspectiveOutline(GfxRenderer& renderer, const Rect& bbox, int hL, int hR, bool fill) {
  const int w = bbox.width;
  if (w <= 0 || hL <= 0 || hR <= 0) return;
  const int hMax = std::max(hL, hR);

  if (fill) {
    for (int dx = 0; dx < w; ++dx) {
      const int colH = (w == 1) ? hL : (hL + (hR - hL) * dx / (w - 1));
      if (colH <= 0) continue;
      const int colTop = (hMax - colH) / 2;
      renderer.fillRect(bbox.x + dx, bbox.y + colTop, 1, colH, true);
    }
    return;
  }

  const int topL = (hMax - hL) / 2;
  const int topR = (hMax - hR) / 2;
  const int botL = topL + hL - 1;
  const int botR = topR + hR - 1;
  const int rightX = bbox.x + w - 1;
  renderer.drawLine(bbox.x, bbox.y + topL, rightX, bbox.y + topR, 2, true);
  renderer.drawLine(bbox.x, bbox.y + botL, rightX, bbox.y + botR, 2, true);
  renderer.fillRect(bbox.x, bbox.y + topL, 2, hL, true);
  renderer.fillRect(rightX - 1, bbox.y + topR, 2, hR, true);
}

void CarrouselRenderer::drawCovers(GfxRenderer& renderer, const Rect& area, const std::vector<RecentBook>& recents,
                                   int centerIndex, bool withFrame) {
  const int count = static_cast<int>(recents.size());
  if (count == 0) return;

  const int n = std::min(kVisibleCovers, count);
  int cur = centerIndex;
  if (cur < 0 || cur >= n) cur = 0;

  const CarrouselLayout layout = computeLayout(renderer, area);
  const Rect& center = layout.cover;
  const int sideW = std::max(24, center.width * kSideWidthPctOfCenter / 100);
  const int innerH = center.height * kSideInnerPct / 100;
  const int outerH = center.height * kSideOuterPct / 100;
  const int hMax = innerH;
  const int drawY = center.y + center.height / 2 - hMax / 2;
  const int sideBoxH = hMax + 2;
  const int overlap = sideW * kSideOverlapPct / 100;
  const int farStep = sideW * kSideFarStepPct / 100;

  const int leftNearX = center.x - sideW + overlap;
  const int leftFarX = leftNearX - farStep;
  const int rightNearX = center.x + center.width - overlap;
  const int rightFarX = rightNearX + farStep;

  const int idxLeftNear = (cur + n - 1) % n;
  const int idxLeftFar = (cur + n - 2) % n;
  const int idxRightNear = (cur + 1) % n;
  const int idxRightFar = (cur + 2) % n;

  if (n >= 5) {
    drawPerspectiveCoverInto(renderer, Rect{leftFarX, drawY, sideW, sideBoxH}, recents[idxLeftFar], innerH, outerH,
                             HomeRenderer::kThumbnailCoverHeight);
  }
  if (n >= 4) {
    drawPerspectiveCoverInto(renderer, Rect{rightFarX, drawY, sideW, sideBoxH}, recents[idxRightFar], outerH, innerH,
                             HomeRenderer::kThumbnailCoverHeight);
  }
  if (n >= 2) {
    drawPerspectiveCoverInto(renderer, Rect{leftNearX, drawY, sideW, sideBoxH}, recents[idxLeftNear], innerH, outerH,
                             HomeRenderer::kThumbnailCoverHeight);
  }
  if (n >= 3) {
    drawPerspectiveCoverInto(renderer, Rect{rightNearX, drawY, sideW, sideBoxH}, recents[idxRightNear], outerH, innerH,
                             HomeRenderer::kThumbnailCoverHeight);
  }

  if (withFrame) {
    constexpr int kCenterFrame = 5;
    const bool clearPlane = renderer.getRenderMode() != GfxRenderer::BW;
    renderer.fillRect(center.x - kCenterFrame, center.y - kCenterFrame, center.width + 2 * kCenterFrame,
                      center.height + 2 * kCenterFrame, clearPlane);
  }
  drawCoverInto(renderer, center, recents[cur], HomeRenderer::kHeroCoverHeight);

  const int8_t centerProgress = HomeProgressCache::getInstance().getProgress(recents[cur].path);
  HomeRenderer::drawCoverProgressOverlay(renderer, center.x, center.y, center.width, center.height, centerProgress);
}

void CarrouselRenderer::drawFull(GfxRenderer& renderer, const Rect& area, const std::vector<RecentBook>& recents,
                                 int centerIndex, bool focused) {
  const int count = static_cast<int>(recents.size());
  if (count == 0) {
    const int cy = area.y + area.height / 2 - renderer.getLineHeight(UI_12_FONT_ID);
    renderer.drawCenteredText(UI_12_FONT_ID, cy, tr(STR_NO_OPEN_BOOK), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, cy + renderer.getLineHeight(UI_12_FONT_ID) + 8, tr(STR_BROWSE_TO_OPEN));
    return;
  }

  drawCovers(renderer, area, recents, centerIndex, /*withFrame=*/true);

  int cur = centerIndex;
  const int n = std::min(kVisibleCovers, count);
  if (cur < 0 || cur >= n) cur = 0;
  const CarrouselLayout layout = computeLayout(renderer, area);
  const RecentBook& centerBook = recents[cur];

  const time_t nowEpoch = time(nullptr);
  const auto& global = StatsManager.getGlobal();
  const bool clockOk = stats::epochValid(static_cast<int64_t>(nowEpoch)) || global.lastSyncedDay != 0;
  drawBookInfo(renderer, area, centerBook, !clockOk);

  if (layout.hasCards) {
    drawStatCards(renderer, layout.cards);
  }
  (void)focused;
}

void CarrouselRenderer::drawSelectionOnly(GfxRenderer& renderer, const Rect& area,
                                          const std::vector<RecentBook>& recents, int centerIndex, bool focused) {
  if (recents.empty()) return;
  const Rect center = centerCoverRect(renderer, area);
  drawSelectionRing(renderer, center, focused);
  (void)centerIndex;
}

void CarrouselRenderer::drawSelectionRing(GfxRenderer& renderer, const Rect& coverRect, bool present) {
  if (!present) return;
  const int r0 = HomeRenderer::kCoverCornerRadius + kSelectionPadding;
  const int r1 = HomeRenderer::kCoverCornerRadius + kSelectionPadding + kSelectionGap;
  const int innerX = coverRect.x - kSelectionPadding;
  const int innerY = coverRect.y - kSelectionPadding;
  const int innerW = coverRect.width + 2 * kSelectionPadding;
  const int innerH = coverRect.height + 2 * kSelectionPadding;
  renderer.drawRoundedRect(innerX, innerY, innerW, innerH, 3, r0, true);
  const int outerX = coverRect.x - kSelectionPadding - kSelectionGap;
  const int outerY = coverRect.y - kSelectionPadding - kSelectionGap;
  const int outerW = coverRect.width + 2 * (kSelectionPadding + kSelectionGap);
  const int outerH = coverRect.height + 2 * (kSelectionPadding + kSelectionGap);
  renderer.drawRoundedRect(outerX, outerY, outerW, outerH, 1, r1, true);
}

void CarrouselRenderer::drawBookInfo(GfxRenderer& renderer, const Rect& area, const RecentBook& book, bool showNudge) {
  const Rect center = centerCoverRect(renderer, area);
  const int width = area.width - 2 * kCardSidePadding;
  const int x = area.x + kCardSidePadding;
  const int titleY = center.y + center.height + kInfoGapTop;
  const int subY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + 2;

  if (!book.title.empty()) {
    const std::string title = renderer.truncatedText(UI_12_FONT_ID, book.title.c_str(), width, EpdFontFamily::BOLD);
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (width - tw) / 2, titleY, title.c_str(), true, EpdFontFamily::BOLD);
  }

  char leftBuf[48];
  bool haveLeft = false;
  const int8_t percent = HomeProgressCache::getInstance().getProgress(book.path);
  const BookStatEntry* stat = findStatsByPath(book.path);
  if (stat && stat->totalReadingMs > 0 && percent > 0 && percent < 100) {
    const uint32_t remainingPct = 100u - static_cast<uint32_t>(percent);
    const uint64_t remainingMs =
        static_cast<uint64_t>(stat->totalReadingMs) * remainingPct / static_cast<uint32_t>(percent);
    char raw[32];
    formatTimeApprox(raw, sizeof(raw), remainingMs);
    std::snprintf(leftBuf, sizeof(leftBuf), "%s: ~%s", tr(STR_HERO_EST_LEFT), raw);
    haveLeft = true;
  }

  if (haveLeft) {
    const int lw = renderer.getTextWidth(UI_10_FONT_ID, leftBuf);
    renderer.drawText(UI_10_FONT_ID, x + (width - lw) / 2, subY, leftBuf, true);
  } else if (showNudge) {
    const std::string nudge = renderer.truncatedText(UI_10_FONT_ID, tr(STR_CARROUSEL_SYNC_NUDGE), width);
    const int nw = renderer.getTextWidth(UI_10_FONT_ID, nudge.c_str());
    renderer.drawText(UI_10_FONT_ID, x + (width - nw) / 2, subY, nudge.c_str(), true);
  }
}

void CarrouselRenderer::drawStatCards(GfxRenderer& renderer, const Rect& rect) {
  const auto& global = StatsManager.getGlobal();
  const time_t nowEpoch = time(nullptr);
  const bool clockOk = stats::epochValid(static_cast<int64_t>(nowEpoch)) || global.lastSyncedDay != 0;
  const uint16_t today =
      stats::epochValid(static_cast<int64_t>(nowEpoch))
          ? stats::dayNumber(static_cast<int64_t>(nowEpoch), stats::utcOffsetSeconds(SETTINGS.clockUtcOffsetQ))
          : 0;

  const int cardW = (rect.width - 2 * kCardSidePadding - 2 * kCardGap) / 3;
  const int cardH = rect.height - 8;
  const int y = rect.y + 4;
  const int x0 = rect.x + kCardSidePadding;
  const int lh10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int valueLh = renderer.getLineHeight(NOTOSANS_16_FONT_ID);

  auto cardX = [&](int i) { return x0 + i * (cardW + kCardGap); };
  for (int i = 0; i < 3; ++i) {
    renderer.drawRoundedRect(cardX(i), y, cardW, cardH, 1, kCardCornerRadius, true);
  }

  auto drawNumberCard = [&](int cx, const char* value, const char* label) {
    const int blockH = valueLh + 2 + lh10;
    const int top = y + (cardH - blockH) / 2;
    const int vw = renderer.getTextWidth(NOTOSANS_16_FONT_ID, value, EpdFontFamily::BOLD);
    renderer.drawText(NOTOSANS_16_FONT_ID, cx + (cardW - vw) / 2, top, value, true, EpdFontFamily::BOLD);
    const std::string lbl = renderer.truncatedText(UI_10_FONT_ID, label, cardW - 6);
    const int lw = renderer.getTextWidth(UI_10_FONT_ID, lbl.c_str());
    renderer.drawText(UI_10_FONT_ID, cx + (cardW - lw) / 2, top + valueLh + 2, lbl.c_str(), true);
  };

  char buf[32];

  if (clockOk) {
    const uint16_t live = stats::streakAlive(global, today) ? global.currentStreakDays : 0;
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(live));
    drawNumberCard(cardX(0), buf, tr(STR_STATS_STREAK));
  } else {
    std::snprintf(buf, sizeof(buf), "%uh", static_cast<unsigned>(global.totalReadingMs / 3600000UL));
    drawNumberCard(cardX(0), buf, tr(STR_STATS_HOURS));
  }

  if (clockOk) {
    const uint32_t todayMin = stats::sumLastNDays(global, today, 1);
    std::snprintf(buf, sizeof(buf), "%u/%u", static_cast<unsigned>(todayMin), static_cast<unsigned>(global.goalTarget));
    drawNumberCard(cardX(1), buf, tr(STR_STATS_TODAY));
  } else {
    std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(global.totalBooksFinished));
    drawNumberCard(cardX(1), buf, tr(STR_STATS_BOOKS));
  }

  {
    const int cx = cardX(2);
    const uint8_t stage = stats::petStageForXp(global.petXp);
    const CatSprite& sprite = *kPetVisuals[stage].sprite;
    std::snprintf(buf, sizeof(buf), "%s %s %u", I18N.get(kPetVisuals[stage].nameKey), tr(STR_STATS_PET_LEVEL),
                  static_cast<unsigned>(stats::petLevelForXp(global.petXp)));
    const int smallLh = renderer.getLineHeight(SMALL_FONT_ID);
    constexpr int kXpBarH = 5;
    constexpr int kXpBarPad = 8;
    const int barY = y + cardH - kCardInnerPad - kXpBarH;
    const int labelY = barY - 3 - smallLh;
    const std::string lbl = renderer.truncatedText(SMALL_FONT_ID, buf, cardW - 6);
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, lbl.c_str());
    const int spriteTop = y + kCardInnerPad;
    const int availH = (labelY - 2) - spriteTop;
    int box = std::min(cardW - 2 * kCardInnerPad, availH);
    if (box < 8) box = 8;
    const int boxX = cx + (cardW - box) / 2;
    const int boxY = spriteTop + (availH - box) / 2;
    drawPetSpriteScaled(renderer, sprite, boxX, boxY, box);
    renderer.drawText(SMALL_FONT_ID, cx + (cardW - lw) / 2, labelY, lbl.c_str(), true);

    const uint16_t floorXp = stats::petXpFloorForStage(stage);
    const uint16_t nextXp = stats::petXpNextForStage(stage);
    int fillPct = 100;
    if (nextXp > floorXp) {
      const uint32_t into = static_cast<uint32_t>(global.petXp - floorXp) * 100u;
      fillPct = static_cast<int>(std::min<uint32_t>(100u, into / static_cast<uint32_t>(nextXp - floorXp)));
    }
    const int barX = cx + kXpBarPad;
    const int barW = cardW - 2 * kXpBarPad;
    renderer.drawRoundedRect(barX, barY, barW, kXpBarH, 1, kXpBarH / 2, true);
    const int fillW = barW * fillPct / 100;
    if (fillW > 0) renderer.fillRect(barX, barY, fillW, kXpBarH, true);
  }
}
