#pragma once

#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "activities/Activity.h"

// Drill-in viewer for a series stack on the home screen. Shows every book in
// the stack as a thumbnail grid; Confirm opens the highlighted book, Back
// returns to Home. The activity is push-only: it never replaces Home, so the
// previous focus state is preserved when the user returns.
class SeriesViewerActivity final : public Activity {
 public:
  SeriesViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string seriesTitle,
                       std::vector<RecentBook> books)
      : Activity("SeriesViewer", renderer, mappedInput), seriesTitle(std::move(seriesTitle)), books(std::move(books)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string seriesTitle;
  std::vector<RecentBook> books;
  int focusIndex = 0;
  // Top visible row index. Auto-adjusts in loop() when focus moves past the
  // visible window so the focused tile is always on-screen.
  int scrollRow = 0;

  // Framebuffer snapshot taken right after the full grid is drawn (covers,
  // titles, badges, chrome) and BEFORE the focus border. On focus-only moves
  // we restore this buffer, draw the new focus border, and FAST_REFRESH --
  // avoiding the SD reads / BMP decodes that dominate full-render cost.
  uint8_t* coverBuffer = nullptr;
  bool coverBufferStored = false;
  // Tracks the scrollRow whose pixels are in coverBuffer. A scroll change
  // invalidates the snapshot, forcing a full repaint.
  int lastScrollRow = -1;
  int lastFocusIndex = -1;

  static constexpr int kCols = 3;

  int rowOf(int idx) const { return idx / kCols; }
  int colOf(int idx) const { return idx % kCols; }
  int rowCount() const;
  int countInRow(int row) const;
  void clampFocus();
  // How many full rows of cells fit into the content area, given the
  // screen height and kCellH. Computed from the actual screen so the
  // viewer scales with future layout tweaks.
  int visibleRows() const;
  int gridTopY() const;
  int gridBottomY() const;
  // Snap scrollRow so the row containing focusIndex stays in view.
  void clampScroll();
  // Vertical pixels between tile tops; expands beyond cell content so the
  // visible rows distribute available slack evenly (no blank band below the
  // last row).
  int rowStride() const;
  // Draws the 2-px double border around the focused cover.
  void drawFocusBorder() const;

  // Sort `books` by series order and pre-load HomeProgressCache entries.
  // Called from onEnter and runScan after the book list is set.
  void sortAndPreload();

  // Framebuffer snapshot helpers, mirroring HomeActivity's pattern. Snapshot
  // captures the grid + chrome WITHOUT the focus border, so focus-only moves
  // restore + redraw the border instead of re-rendering every tile from SD.
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();

  // Long-press Back handler: re-runs the SD-card scan, overwrites the
  // per-series book cache, force-regenerates every cover thumbnail at the
  // series-viewer size (so missing/stale covers are redrawn), and
  // refreshes the grid.
  void runScan();
};
