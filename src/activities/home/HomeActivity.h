#pragma once
#include <cstdint>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"  // for Rect

// Tile in the recents area. A tile is either a single book or a "series
// stack" of multiple books grouped by shared metadata. bookIndices[0] is the
// most-recently-read member -- the one drawn on top and opened on Confirm
// (or, for stacks, the first item in the drill-in viewer).
struct HomeTile {
  std::vector<int> bookIndices;
  // Override for the count badge drawn on a series stack thumbnail. -1 means
  // "use bookIndices.size()". Populated from the persistent series-count
  // cache so the badge reflects the real on-disk total -- not just the
  // members surviving in `recentBooks` after FIFO eviction.
  int displayStackSize = -1;
};

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
  // Series-grouped view of recentBooks. Each tile is a single book or a
  // stack. tiles[0] is the hero; tiles[1..] fill the thumbnail rows.
  std::vector<HomeTile> tiles;
  bool recentsLoaded = false;
  bool recentsLoading = false;
  bool firstRenderDone = false;

  // Cached framebuffer of the home with no selection drawn. Used to repaint
  // selection borders without re-decoding covers on every Up/Down/Left/Right.
  uint8_t* coverBuffer = nullptr;
  bool coverBufferStored = false;

  // Lazy progress lookup for every visible book (hero + thumbnails). Filled
  // in on the render pass after covers + book.bin are available on disk.
  bool progressLoaded = false;

  FocusRow focusRow = FOCUS_HERO;
  int focusIndex = 0;  // column within the focused row

  // --- Lifecycle helpers ---
  void loadRecentBooks(int max);
  void loadRecentCovers();
  // Build `tiles` from `recentBooks` by grouping books that share series
  // metadata (calibre / EPUB 3 collection). When two books both lack series
  // metadata but live in the same parent folder, they are grouped under that
  // folder name as a fallback. Order is preserved by first-seen.
  void buildTiles();

  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();

  // --- Navigation ---
  void moveFocus(int dRow, int dCol);
  bool rowIsFocusable(FocusRow row) const;
  int rowItemCount(FocusRow row) const;
  void openFocused();
  // Returns the book under the current focus when the focused tile holds
  // exactly one book; returns nullptr for menu rows or series stacks. Used
  // by the long-press "Remove from recents" flow which only operates on
  // single-book tiles (series-stack removal is a separate UX question).
  const RecentBook* focusedSingleBook() const;
  // Long-press Confirm on a single-book tile. Pushes ConfirmationActivity;
  // on accept, removes the book from RecentBooksStore and reloads home.
  void confirmRemoveFocusedBook();

  // --- Rendering ---
  Rect heroRect() const;
  Rect dividerRect() const;
  Rect sectionLabelRect() const;
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
