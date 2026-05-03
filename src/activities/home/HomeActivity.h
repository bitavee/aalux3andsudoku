#pragma once
#include <cstdint>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"  // for Rect

class HomeActivity final : public Activity {
  // Logical rows on the home screen, in vertical order.
  enum FocusRow {
    FOCUS_HERO = 0,
    FOCUS_THUMBS_R1 = 1,
    FOCUS_THUMBS_R2 = 2,
    FOCUS_MENU = 3,
  };

  // Up to 9 recents: index 0 = hero, 1..4 = thumbnail row 1, 5..8 = thumbnail row 2.
  std::vector<RecentBook> recentBooks;
  bool recentsLoaded = false;
  bool recentsLoading = false;
  bool firstRenderDone = false;

  // Cached framebuffer of the home with no selection drawn. Used to repaint
  // selection borders without re-decoding covers on every Up/Down/Left/Right.
  uint8_t* coverBuffer = nullptr;
  bool coverBufferStored = false;

  // Lazy hero progress (filled in on second render after covers + book.bin
  // are available on disk).
  bool heroProgressLoaded = false;

  FocusRow focusRow = FOCUS_HERO;
  int focusIndex = 0;  // column within the focused row

  // --- Lifecycle helpers ---
  void loadRecentBooks(int max);
  void loadRecentCovers();

  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();

  // --- Navigation ---
  void moveFocus(int dRow, int dCol);
  bool rowIsFocusable(FocusRow row) const;
  int rowItemCount(FocusRow row) const;
  void openFocused();

  // --- Rendering ---
  Rect heroRect() const;
  Rect dividerRect() const;
  Rect thumbsRow1Rect() const;
  Rect thumbsRow2Rect() const;
  Rect menuRect() const;
  Rect focusedItemRect() const;
  void renderFull();

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Home", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
