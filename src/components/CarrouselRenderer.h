#pragma once

// Cover-flow home style ("Carrousel"). The cover-flow layout math, the
// outside-in five-cover slot model, the slot-keyed packed-1bpp side-tile cache,
// and the independent selection-ring redraw are ported from CrumBLE's
// LyraFlowTheme (MIT), itself a fork of CrossInk.
//
// SPDX-License-Identifier: MIT
// Portions Copyright (c) CrumBLE / CrossInk contributors.
// See https://github.com/imshentastic/CrumBLE (LyraFlowTheme).

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;
struct RecentBook;
struct Rect;

class CarrouselRenderer {
 public:
  static constexpr int kVisibleCovers = 5;
  static constexpr int kSideSlots = 4;

  CarrouselRenderer() = default;
  ~CarrouselRenderer();

  CarrouselRenderer(const CarrouselRenderer&) = delete;
  CarrouselRenderer& operator=(const CarrouselRenderer&) = delete;

  void reset();

  void drawFull(GfxRenderer& renderer, const Rect& area, const std::vector<RecentBook>& recents, int centerIndex,
                bool focused);

  void drawSelectionOnly(GfxRenderer& renderer, const Rect& area, const std::vector<RecentBook>& recents,
                         int centerIndex, bool focused);

  static Rect centerCoverRect(GfxRenderer& renderer, const Rect& area);

 private:
  struct SideTile {
    uint8_t* packed = nullptr;
    size_t bytes = 0;
    int width = 0;
    int height = 0;
    std::string bookPath;
  };

  void freeTiles();
  Rect cardsRect(const Rect& area) const;
  bool cardsVisible(GfxRenderer& renderer, const Rect& area) const;

  void drawCoverInto(GfxRenderer& renderer, const Rect& dst, const RecentBook& book, int targetHeight);
  void drawSideTileCached(GfxRenderer& renderer, int slot, const Rect& bbox, const RecentBook& book, int hL, int hR,
                          int targetHeight);
  void drawPerspectiveCoverInto(GfxRenderer& renderer, const Rect& bbox, const RecentBook& book, int hL, int hR,
                                int targetHeight);
  static void drawPerspectiveOutline(GfxRenderer& renderer, const Rect& bbox, int hL, int hR, bool fill);

  void drawBookInfo(GfxRenderer& renderer, const Rect& area, const RecentBook& book, bool showNudge);
  void drawStatCards(GfxRenderer& renderer, const Rect& rect);
  void drawSelectionRing(GfxRenderer& renderer, const Rect& coverRect, bool present);

  SideTile tiles[kSideSlots];
};
