#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "BookshelfCache.h"

// Full-library grid view. Replaces the legacy FileBrowserActivity: instead
// of surfacing folders/files, the bookshelf shows every book on the SD card
// as a cover tile, with multi-book series collapsed into Home-style stack
// tiles. Backed by a persisted cache (BookshelfCache); the SD card is only
// re-walked on cold open, manual long-press refresh, or after an external
// invalidation (File Transfer / OPDS download).
class BookshelfActivity final : public Activity {
 public:
  explicit BookshelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Bookshelf", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // One renderable cell. `entryIndices[0]` is the seed (cover + label
  // source). `entryIndices.size() > 1` means a series stack: count badge =
  // size, ghost layers behind the seed cover, drill-in opens SeriesViewer.
  struct Tile {
    std::vector<int> entryIndices;
    std::string label;
    std::string thumbPath;
  };

  enum class Mode {
    Loading,  // showing the "Refreshing library…" popup; input disabled
    Grid,     // cache loaded with at least one book
    Empty,    // cache loaded but no books found on the SD card
  };

  std::vector<BookshelfCache::Entry> entries;
  std::vector<Tile> tiles;
  Mode mode = Mode::Loading;

  int focusIndex = 0;
  int scrollRow = 0;
  // Cached scroll/focus values for the framebuffer snapshot. When focus
  // changes within the same scroll window, we restore the snapshot and
  // stamp a fresh focus border instead of re-decoding every cover.
  int lastScrollRow = -1;
  int lastFocusIndex = -1;

  uint8_t* coverBuffer = nullptr;
  bool coverBufferStored = false;

  // Set true when the activity should fire the one-time refresh-hint tooltip
  // on the next render. Cleared after the tooltip dismisses; persists across
  // power cycles via SETTINGS.bookshelfRefreshHintSeen.
  bool tooltipPending = false;

  // --- Lifecycle helpers ---
  void loadOrScan();
  void runScan();
  void buildTiles();
  bool isFocusOnSeries() const;

  // --- Geometry ---
  int rowCount() const;
  int rowOf(int idx) const;
  int colOf(int idx) const;
  int visibleRows() const;
  int gridTopY() const;
  int gridBottomY() const;
  int rowStride() const;

  void clampFocus();
  void clampScroll();

  // --- Rendering ---
  void renderGrid();
  void renderEmpty();
  void drawFocusBorder() const;
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
  void showFirstTimeTooltip();

  // --- Input handlers ---
  void openFocused();
  void confirmDeleteFocused();
  void handleMissingBook(const std::string& path);
};
