#pragma once

#include <array>
#include <cstdint>

#include "activities/Activity.h"

// A self-contained 9x9 Sudoku game screen.
class SudokuActivity final : public Activity {
 public:
  explicit SudokuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sudoku", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  static constexpr int kGridSize = 9;
  static constexpr int kBoxSize = 3;

  enum class Mode { Navigating, EnteringValue };

  // 0 = empty. `given` cells were part of the original puzzle and can't be
  // edited; the rest are player-filled.
  std::array<std::array<uint8_t, kGridSize>, kGridSize> board{};
  std::array<std::array<bool, kGridSize>, kGridSize> given{};

  int cursorRow = 0;
  int cursorCol = 0;
  Mode mode = Mode::Navigating;
  int padSelection = 0;  // 0-8 = digits 1-9, 9 = clear (X)
  bool solved = false;

  void newGame();
  void generatePuzzle();  // fills `board`/`given` with a solvable puzzle
  bool isValidPlacement(int row, int col, uint8_t value) const;
  bool checkComplete();  // true if board is full and every rule holds; sets `solved`

  void renderGrid(int gridX, int gridY, int cellSize) const;
  void renderNumberPad(int padY, int padX, int cellSize) const;

  void moveCursor(int dRow, int dCol);
  void movePadSelection(int delta);
  void placeValue(uint8_t value);  // 0 = clear cell
};
