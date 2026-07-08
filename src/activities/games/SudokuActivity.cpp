#include "activities/games/SudokuActivity.h"

#include <cstdlib>
#include <utility>

#include "HalDisplay.h"
#include "MappedInputManager.h"
#include "fontIds.h"

using Button = MappedInputManager::Button;

void SudokuActivity::onEnter() {
  Activity::onEnter();
  newGame();
  requestUpdate(true);
}

void SudokuActivity::newGame() {
  generatePuzzle();
  cursorRow = 0;
  cursorCol = 0;
  mode = Mode::Navigating;
  padSelection = 0;
  solved = false;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void SudokuActivity::loop() {
  if (mode == Mode::Navigating) {
    if (mappedInput.wasReleased(Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasReleased(Button::Up)) {
      moveCursor(-1, 0);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(Button::Down)) {
      moveCursor(1, 0);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(Button::Left)) {
      moveCursor(0, -1);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(Button::Right)) {
      moveCursor(0, 1);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(Button::Confirm)) {
      if (given[cursorRow][cursorCol]) return;  // clues aren't editable
      mode = Mode::EnteringValue;
      padSelection = (board[cursorRow][cursorCol] == 0) ? 0 : board[cursorRow][cursorCol] - 1;
      requestUpdate();
      return;
    }
    return;
  }

  // Mode::EnteringValue — number pad is active for the focused cell.
  if (mappedInput.wasReleased(Button::Back)) {
    mode = Mode::Navigating;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(Button::Left)) {
    movePadSelection(-1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(Button::Right)) {
    movePadSelection(1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(Button::Confirm)) {
    placeValue(padSelection == 9 ? 0 : static_cast<uint8_t>(padSelection + 1));
    mode = Mode::Navigating;
    checkComplete();
    requestUpdate();
    return;
  }
}

void SudokuActivity::moveCursor(int dRow, int dCol) {
  cursorRow = (cursorRow + dRow + kGridSize) % kGridSize;
  cursorCol = (cursorCol + dCol + kGridSize) % kGridSize;
}

void SudokuActivity::movePadSelection(int delta) {
  padSelection = (padSelection + delta + 10) % 10;  // 0-8 digits, 9 = clear
}

void SudokuActivity::placeValue(uint8_t value) {
  if (given[cursorRow][cursorCol]) return;
  board[cursorRow][cursorCol] = value;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void SudokuActivity::render(RenderLock&& lock) {
  const int screenW = renderer.getScreenWidth();

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 8, "Sudoku", true, EpdFontFamily::BOLD);
  renderer.drawLine(0, 34, screenW, 34);

  const int gridPixels = screenW - 40;
  const int cellSize = gridPixels / kGridSize;
  const int gridX = (screenW - cellSize * kGridSize) / 2;
  const int gridY = 46;

  renderGrid(gridX, gridY, cellSize);
  renderNumberPad(gridY + cellSize * kGridSize + 16, gridX, cellSize);

  if (solved) {
    renderer.drawCenteredText(UI_10_FONT_ID, gridY + cellSize * kGridSize + 60, "Solved!", true,
                              EpdFontFamily::BOLD);
  }

  renderer.displayBuffer(HalDisplay::RefreshMode::FAST_REFRESH);
}

void SudokuActivity::renderGrid(int gridX, int gridY, int cellSize) const {
  // Thin per-cell lines.
  for (int i = 0; i <= kGridSize; ++i) {
    const bool bold = (i % kBoxSize == 0);
    const int lineWidth = bold ? 2 : 1;
    renderer.drawLine(gridX + i * cellSize, gridY, gridX + i * cellSize, gridY + cellSize * kGridSize, lineWidth,
                      true);
    renderer.drawLine(gridX, gridY + i * cellSize, gridX + cellSize * kGridSize, gridY + i * cellSize, lineWidth,
                      true);
  }

  // Digits + cursor highlight.
  for (int r = 0; r < kGridSize; ++r) {
    for (int c = 0; c < kGridSize; ++c) {
      const int x = gridX + c * cellSize;
      const int y = gridY + r * cellSize;
      const bool isCursor = (mode == Mode::Navigating && r == cursorRow && c == cursorCol);

      if (isCursor) {
        renderer.drawRect(x + 2, y + 2, cellSize - 4, cellSize - 4, 2, true);
      }

      const uint8_t value = board[r][c];
      if (value != 0) {
        char label[2] = {static_cast<char>('0' + value), '\0'};
        const int fontId = given[r][c] ? UI_12_FONT_ID : UI_10_FONT_ID;
        const int textW = renderer.getTextWidth(fontId, label);
        const int textH = renderer.getFontAscenderSize(fontId);
        renderer.drawText(fontId, x + (cellSize - textW) / 2, y + (cellSize - textH) / 2, label, true,
                          given[r][c] ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
    }
  }
}

void SudokuActivity::renderNumberPad(int padY, int padX, int cellSize) const {
  if (mode != Mode::EnteringValue) {
    renderer.drawCenteredText(UI_10_FONT_ID, padY, "Confirm to edit a cell");
    return;
  }

  // 10 slots: digits 1-9 then "X" (clear), each in a cellSize-wide box
  // starting at the grid's left edge so it visually lines up underneath.
  const int padCellW = (cellSize * kGridSize) / 10;
  for (int i = 0; i < 10; ++i) {
    const int x = padX + i * padCellW;
    const bool selected = (i == padSelection);
    if (selected) {
      renderer.fillRect(x + 1, padY, padCellW - 2, padCellW - 2, true);
    } else {
      renderer.drawRect(x + 1, padY, padCellW - 2, padCellW - 2, 1, true);
    }

    char label[2] = {i < 9 ? static_cast<char>('1' + i) : 'X', '\0'};
    const int textW = renderer.getTextWidth(UI_10_FONT_ID, label);
    // When the box is filled (selected), draw the digit inverted (white-on-black).
    renderer.drawText(UI_10_FONT_ID, x + (padCellW - textW) / 2, padY + 4, label, !selected);
  }
}

// ---------------------------------------------------------------------------
// Puzzle logic
// ---------------------------------------------------------------------------

bool SudokuActivity::isValidPlacement(int row, int col, uint8_t value) const {
  if (value == 0) return true;
  for (int i = 0; i < kGridSize; ++i) {
    if (i != col && board[row][i] == value) return false;
    if (i != row && board[i][col] == value) return false;
  }
  const int boxRow = (row / kBoxSize) * kBoxSize;
  const int boxCol = (col / kBoxSize) * kBoxSize;
  for (int r = boxRow; r < boxRow + kBoxSize; ++r) {
    for (int c = boxCol; c < boxCol + kBoxSize; ++c) {
      if ((r != row || c != col) && board[r][c] == value) return false;
    }
  }
  return true;
}

bool SudokuActivity::checkComplete() {
  for (int r = 0; r < kGridSize; ++r) {
    for (int c = 0; c < kGridSize; ++c) {
      if (board[r][c] == 0 || !isValidPlacement(r, c, board[r][c])) {
        solved = false;
        return false;
      }
    }
  }
  solved = true;
  return true;
}

namespace {
// Standard randomized backtracking fill — fast enough for a one-time
// 9x9 generation on boot, no need for anything fancier on a 380KB device.
bool fillCell(std::array<std::array<uint8_t, 9>, 9>& b, int pos) {
  if (pos == 81) return true;
  const int row = pos / 9;
  const int col = pos % 9;

  uint8_t candidates[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  for (int i = 8; i > 0; --i) std::swap(candidates[i], candidates[std::rand() % (i + 1)]);

  for (uint8_t v : candidates) {
    bool ok = true;
    for (int i = 0; i < 9 && ok; ++i) {
      if (b[row][i] == v || b[i][col] == v) ok = false;
    }
    const int br = (row / 3) * 3;
    const int bc = (col / 3) * 3;
    for (int r = br; r < br + 3 && ok; ++r) {
      for (int c = bc; c < bc + 3 && ok; ++c) {
        if (b[r][c] == v) ok = false;
      }
    }

    if (!ok) continue;
    b[row][col] = v;
    if (fillCell(b, pos + 1)) return true;
    b[row][col] = 0;
  }
  return false;
}
}  // namespace

void SudokuActivity::generatePuzzle() {
  for (auto& row : board) row.fill(0);
  fillCell(board, 0);

  // Dig holes for a medium-difficulty puzzle (~45 givens remain). This does
  // not guarantee a unique solution for the remaining puzzle — fine for a
  // casual e-reader distraction, but flag it if you want a "no ambiguity"
  // solver-count check added on top later.
  for (auto& row : given) row.fill(true);
  int toRemove = 36;
  while (toRemove > 0) {
    const int r = std::rand() % kGridSize;
    const int c = std::rand() % kGridSize;
    if (given[r][c]) {
      given[r][c] = false;
      board[r][c] = 0;
      --toRemove;
    }
  }
}
