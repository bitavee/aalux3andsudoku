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
      : Activity("SeriesViewer", renderer, mappedInput),
        seriesTitle(std::move(seriesTitle)),
        books(std::move(books)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string seriesTitle;
  std::vector<RecentBook> books;
  int focusIndex = 0;
  // Top visible row index. Auto-adjusts in loop() when focus moves past the
  // visible window so the focused tile is always on-screen.
  int scrollRow = 0;

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

  // Sort `books` by series order and pre-load HomeProgressCache entries.
  // Called from onEnter and runScan after the book list is set.
  void sortAndPreload();

  // Long-press Back handler: re-runs the SD-card scan, overwrites the
  // per-series book cache, and refreshes the grid.
  void runScan();
};
