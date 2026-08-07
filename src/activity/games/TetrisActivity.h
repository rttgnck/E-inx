#pragma once

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <cstdio>
#include <cstring>
#include <functional>

#include "activity/Activity.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"

class TetrisActivity final : public Activity {
 public:
  TetrisActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onBack)
      : Activity("Tetris", renderer, mappedInput), onBack_(onBack) {}

  void onEnter() override {
    Activity::onEnter();
    resetGame();
    render(true);
  }

  void onExit() override { Activity::onExit(); }

  void loop() override {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= kExitHoldMs) {
      if (!exitTriggered_) {
        exitTriggered_ = true;
        onBack_();
      }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      exitTriggered_ = false;
    }

    if (gameOver_) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
          mappedInput.wasPressed(MappedInputManager::Button::Up)) {
        resetGame();
        render(true);
      }
      return;
    }

    bool moved = false;

    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      moved = tryMove(-1, 0);
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      moved = tryMove(1, 0);
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      moved = tryRotate(-1);
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      moved = tryRotate(1);
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
      moved = hardDrop();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
        mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
      holdPiece();
      moved = true;
    }

    const unsigned long now = millis();
    if (now - lastDropMs_ >= dropIntervalMs()) {
      if (tryMove(0, 1)) {
        moved = true;
      } else {
        lockPiece();
        moved = true;
      }
      lastDropMs_ = now;
    }

    if (moved) {
      render(false);
    }
  }

  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  static constexpr int kBoardWidth = 10;
  static constexpr int kBoardHeight = 20;
  static constexpr int kHiddenRows = 2;
  static constexpr int kBoardRows = kBoardHeight + kHiddenRows;
  static constexpr int kCellSize = 20;
  static constexpr int kBoardPixelWidth = kBoardWidth * kCellSize;
  static constexpr int kBoardPixelHeight = kBoardHeight * kCellSize;
  static constexpr unsigned long kExitHoldMs = 800;
  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);

  enum class PieceType : uint8_t { I = 0, O, T, S, Z, L, J, Count };

  struct ActivePiece {
    PieceType type = PieceType::I;
    int rotation = 0;
    int x = 0;
    int y = 0;
  };

  std::function<void()> onBack_;
  uint8_t board_[kBoardWidth * kBoardRows]{};
  ActivePiece current_{};
  PieceType next_ = PieceType::I;
  PieceType hold_ = PieceType::Count;
  bool holdUsed_ = false;
  int score_ = 0;
  int bestScore_ = 0;
  int level_ = 1;
  int lines_ = 0;
  bool gameOver_ = false;
  bool exitTriggered_ = false;
  unsigned long lastDropMs_ = 0;
  bool firstRender_ = true;

  static inline constexpr uint8_t kShapeRows[7][4][4][4] = {
      {// I
       {{0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {{0, 0, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0}},
       {{0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}},
       {{0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}}},
      {// O
       {{0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {{0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {{0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {{0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}},
      {// T
       {{0, 1, 0, 0}, {1, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {{0, 1, 0, 0}, {0, 1, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}},
       {{0, 0, 0, 0}, {1, 1, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}},
       {{0, 1, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}}},
      {// S
       {{0, 1, 1, 0}, {1, 1, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {{0, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 0, 0}},
       {{0, 0, 0, 0}, {0, 1, 1, 0}, {1, 1, 0, 0}, {0, 0, 0, 0}},
       {{1, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}}},
      {// Z
       {{1, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {{0, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}},
       {{0, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}},
       {{0, 1, 0, 0}, {1, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}}},
      {// L
       {{0, 0, 1, 0}, {1, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {{0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0}},
       {{0, 0, 0, 0}, {1, 1, 1, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}},
       {{1, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}}},
      {// J
       {{1, 0, 0, 0}, {1, 1, 1, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}},
       {{0, 1, 1, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 0, 0, 0}},
       {{0, 0, 0, 0}, {1, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 0, 0}},
       {{0, 1, 0, 0}, {0, 1, 0, 0}, {1, 1, 0, 0}, {0, 0, 0, 0}}},
  };

  void resetGame() {
    std::memset(board_, 0, sizeof(board_));
    if (score_ > bestScore_) bestScore_ = score_;
    score_ = 0;
    level_ = 1;
    lines_ = 0;
    gameOver_ = false;
    exitTriggered_ = false;
    firstRender_ = true;
    hold_ = PieceType::Count;
    holdUsed_ = false;
    next_ = randomPiece();
    spawnPiece();
    lastDropMs_ = millis();
  }

  static PieceType randomPiece() { return static_cast<PieceType>(random(0, 7)); }
  static int spawnColumn(PieceType) { return 3; }

  static bool shapeCell(PieceType type, int rotation, int row, int col) {
    return kShapeRows[static_cast<int>(type)][rotation & 3][row][col] != 0;
  }

  void spawnPiece() {
    current_.type = next_;
    current_.rotation = 0;
    current_.x = spawnColumn(current_.type);
    current_.y = 0;
    next_ = randomPiece();
    holdUsed_ = false;
    if (collides(current_)) gameOver_ = true;
  }

  bool collides(const ActivePiece& piece) const {
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!shapeCell(piece.type, piece.rotation, row, col)) continue;
        const int bx = piece.x + col;
        const int by = piece.y + row;
        if (bx < 0 || bx >= kBoardWidth || by >= kBoardRows) return true;
        if (by >= 0 && board_[by * kBoardWidth + bx] != 0) return true;
      }
    }
    return false;
  }

  bool tryMove(int dx, int dy) {
    ActivePiece moved = current_;
    moved.x += dx;
    moved.y += dy;
    if (collides(moved)) return false;
    current_ = moved;
    return true;
  }

  bool tryRotate(int dir) {
    ActivePiece rotated = current_;
    rotated.rotation = (rotated.rotation + dir + 4) & 3;
    if (!collides(rotated)) {
      current_ = rotated;
      return true;
    }
    static constexpr int kKicks[] = {-1, 1, -2, 2};
    for (const int kick : kKicks) {
      rotated.x = current_.x + kick;
      if (!collides(rotated)) {
        current_ = rotated;
        return true;
      }
    }
    return false;
  }

  bool hardDrop() {
    int dropped = 0;
    while (tryMove(0, 1)) ++dropped;
    score_ += dropped * 2;
    lockPiece();
    return true;
  }

  void holdPiece() {
    if (holdUsed_) return;
    holdUsed_ = true;
    if (hold_ == PieceType::Count) {
      hold_ = current_.type;
      spawnPiece();
    } else {
      PieceType tmp = hold_;
      hold_ = current_.type;
      current_.type = tmp;
      current_.rotation = 0;
      current_.x = spawnColumn(current_.type);
      current_.y = 0;
      if (collides(current_)) gameOver_ = true;
    }
  }

  void lockPiece() {
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!shapeCell(current_.type, current_.rotation, row, col)) continue;
        const int bx = current_.x + col;
        const int by = current_.y + row;
        if (bx >= 0 && bx < kBoardWidth && by >= 0 && by < kBoardRows)
          board_[by * kBoardWidth + bx] = static_cast<uint8_t>(current_.type) + 1;
      }
    }
    clearLines();
    if (!gameOver_) spawnPiece();
    lastDropMs_ = millis();
  }

  void clearLines() {
    int cleared = 0;
    for (int row = kBoardRows - 1; row >= 0; --row) {
      bool full = true;
      for (int col = 0; col < kBoardWidth; ++col) {
        if (board_[row * kBoardWidth + col] == 0) { full = false; break; }
      }
      if (!full) continue;
      ++cleared;
      for (int moveRow = row; moveRow > 0; --moveRow)
        for (int col = 0; col < kBoardWidth; ++col)
          board_[moveRow * kBoardWidth + col] = board_[(moveRow - 1) * kBoardWidth + col];
      for (int col = 0; col < kBoardWidth; ++col) board_[col] = 0;
      ++row;
    }
    if (cleared == 0) return;
    static constexpr int kLineScores[] = {0, 100, 300, 500, 800};
    score_ += kLineScores[cleared] * level_;
    lines_ += cleared;
    level_ = 1 + (lines_ / 10);
  }

  unsigned long dropIntervalMs() const {
    const unsigned long interval = 1000UL - static_cast<unsigned long>(level_ - 1) * 80UL;
    return interval < 200UL ? 200UL : interval;
  }

  int ghostDropRow() const {
    ActivePiece ghost = current_;
    while (true) {
      ActivePiece next = ghost;
      next.y += 1;
      if (collides(next)) break;
      ghost = next;
    }
    return ghost.y;
  }

  // --- Layout matching reference image ---
  // Stats top-left, NEXT top-right, HOLD below NEXT, board centered, controls at bottom
  void computeLayout(int& boardLeft, int& boardTop) const {
    const int sw = renderer.getScreenWidth();
    const int sh = renderer.getScreenHeight();
    boardLeft = (sw - kBoardPixelWidth) / 2;
    boardTop = 50;
    if (boardTop + kBoardPixelHeight + 60 > sh) {
      boardTop = (sh - kBoardPixelHeight - 40) / 2;
      if (boardTop < 10) boardTop = 10;
    }
  }

  void drawCell(int boardLeft, int boardTop, int col, int visRow, bool filled, bool outline = false) const {
    if (visRow < 0 || visRow >= kBoardHeight) return;
    const int px = boardLeft + col * kCellSize + 1;
    const int py = boardTop + visRow * kCellSize + 1;
    const int sz = kCellSize - 2;
    if (filled)
      renderer.rectangle.fill(px, py, sz, sz, kInk);
    else if (outline)
      renderer.rectangle.render(px, py, sz, sz);
  }

  void drawPreviewBox(int x, int y, const char* label, PieceType type) const {
    const int previewCell = 14;
    const int boxW = previewCell * 4 + 10;
    const int boxH = previewCell * 4 + 10;

    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, x, y - 16, label);
    renderer.rectangle.render(x, y, boxW, boxH);

    if (type == PieceType::Count) return;
    for (int row = 0; row < 4; ++row)
      for (int col = 0; col < 4; ++col)
        if (shapeCell(type, 0, row, col))
          renderer.rectangle.fill(x + 5 + col * previewCell + 1, y + 5 + row * previewCell + 1,
                                  previewCell - 2, previewCell - 2, kInk);
  }

  void render(bool forceFullRefresh) {
    int boardLeft = 0, boardTop = 0;
    computeLayout(boardLeft, boardTop);
    const int sw = renderer.getScreenWidth();

    renderer.clearScreen();

    // --- Stats: top-left ---
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Score %d", score_);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, 10, 10, buf);

    std::snprintf(buf, sizeof(buf), "Level %d", level_);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 10, 32, buf);

    std::snprintf(buf, sizeof(buf), "Lines %d", lines_);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 10, 48, buf);

    if (bestScore_ > 0) {
      std::snprintf(buf, sizeof(buf), "Best %d", bestScore_);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 10, 64, buf);
    }

    // --- NEXT preview: top-right ---
    const int previewBoxW = 14 * 4 + 10;
    const int previewX = sw - previewBoxW - 15;
    drawPreviewBox(previewX, 30, "NEXT", next_);

    // --- HOLD preview: below NEXT ---
    drawPreviewBox(previewX, 120, "HOLD", hold_);

    // --- Board: centered ---
    renderer.rectangle.render(boardLeft - 2, boardTop - 2, kBoardPixelWidth + 4, kBoardPixelHeight + 4);

    for (int row = kHiddenRows; row < kBoardRows; ++row)
      for (int col = 0; col < kBoardWidth; ++col)
        if (board_[row * kBoardWidth + col] != 0)
          drawCell(boardLeft, boardTop, col, row - kHiddenRows, true);

    // Ghost piece
    const int ghostY = ghostDropRow();
    if (ghostY != current_.y) {
      for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
          if (shapeCell(current_.type, current_.rotation, row, col))
            drawCell(boardLeft, boardTop, current_.x + col, ghostY + row - kHiddenRows, false, true);
    }

    // Current piece
    for (int row = 0; row < 4; ++row)
      for (int col = 0; col < 4; ++col)
        if (shapeCell(current_.type, current_.rotation, row, col))
          drawCell(boardLeft, boardTop, current_.x + col, current_.y + row - kHiddenRows, true);

    // --- Side button hints (right edge): Drop / Hold ---
    renderer.ui.sideButtonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, nullptr, "Drop", "Hold");

    // --- Front button hints (bottom): Rotate L | Rotate R | Move L | Move R ---
    renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, "Rotate L", "Rotate R", "Move L", "Move R");

    // --- Game over overlay ---
    if (gameOver_) {
      if (score_ > bestScore_) bestScore_ = score_;
      const int oW = 280, oH = 100;
      const int oX = (sw - oW) / 2;
      const int oY = boardTop + (kBoardPixelHeight - oH) / 2;
      renderer.rectangle.fill(oX, oY, oW, oH, static_cast<int>(GfxRenderer::FillTone::Paper));
      renderer.rectangle.render(oX, oY, oW, oH);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, oX + 70, oY + 14, "GAME OVER");
      std::snprintf(buf, sizeof(buf), "Score: %d", score_);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, oX + 80, oY + 42, buf);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, oX + 50, oY + 72, "Press any button to restart");
    }

    renderer.displayBuffer((forceFullRefresh || firstRender_) ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    firstRender_ = false;
  }
};
