#pragma once

/**
 * @file TetrisActivity.h
 * @brief Header-only Tetris game activity for the e-ink reader.
 */

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
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onBack_();
      return;
    }

    if (gameOver_) {
      if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
        onBack_();
      }
      return;
    }

    bool moved = false;

    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      moved = tryMove(-1, 0);
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      moved = tryMove(1, 0);
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (tryMove(0, 1)) {
        score_ += 1;
        moved = true;
      } else {
        lockPiece();
        moved = true;
      }
      lastDropMs_ = millis();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      moved = hardDrop();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      moved = tryRotate();
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
  static constexpr int kCellSize = 22;
  static constexpr int kBoardPixelWidth = kBoardWidth * kCellSize;
  static constexpr int kBoardPixelHeight = kBoardHeight * kCellSize;

  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);
  static constexpr int kPaper = static_cast<int>(GfxRenderer::FillTone::Paper);

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
  int score_ = 0;
  int level_ = 1;
  int lines_ = 0;
  bool gameOver_ = false;
  unsigned long lastDropMs_ = 0;
  bool firstRender_ = true;

  // Each piece stores four rotations in a 4x4 grid (1 = block).
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
    score_ = 0;
    level_ = 1;
    lines_ = 0;
    gameOver_ = false;
    firstRender_ = true;
    next_ = randomPiece();
    spawnPiece();
    lastDropMs_ = millis();
  }

  static PieceType randomPiece() { return static_cast<PieceType>(random(0, 7)); }

  static int spawnColumn(PieceType type) { return type == PieceType::O ? 4 : 3; }

  static bool shapeCell(PieceType type, int rotation, int row, int col) {
    return kShapeRows[static_cast<int>(type)][rotation & 3][row][col] != 0;
  }

  void spawnPiece() {
    current_.type = next_;
    current_.rotation = 0;
    current_.x = spawnColumn(current_.type);
    current_.y = 0;
    next_ = randomPiece();

    if (collides(current_)) {
      gameOver_ = true;
    }
  }

  bool collides(const ActivePiece& piece) const {
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!shapeCell(piece.type, piece.rotation, row, col)) {
          continue;
        }
        const int bx = piece.x + col;
        const int by = piece.y + row;
        if (bx < 0 || bx >= kBoardWidth || by >= kBoardRows) {
          return true;
        }
        if (by < 0) {
          continue;
        }
        if (board_[by * kBoardWidth + bx] != 0) {
          return true;
        }
      }
    }
    return false;
  }

  bool tryMove(int dx, int dy) {
    ActivePiece moved = current_;
    moved.x += dx;
    moved.y += dy;
    if (collides(moved)) {
      return false;
    }
    current_ = moved;
    return true;
  }

  bool tryRotate() {
    ActivePiece rotated = current_;
    rotated.rotation = (rotated.rotation + 1) & 3;
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
    while (tryMove(0, 1)) {
      ++dropped;
    }
    score_ += dropped * 2;
    lockPiece();
    return true;
  }

  void lockPiece() {
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!shapeCell(current_.type, current_.rotation, row, col)) {
          continue;
        }
        const int bx = current_.x + col;
        const int by = current_.y + row;
        if (bx >= 0 && bx < kBoardWidth && by >= 0 && by < kBoardRows) {
          board_[by * kBoardWidth + bx] = static_cast<uint8_t>(current_.type) + 1;
        }
      }
    }

    clearLines();
    if (!gameOver_) {
      spawnPiece();
    }
    lastDropMs_ = millis();
  }

  void clearLines() {
    int cleared = 0;
    for (int row = kBoardRows - 1; row >= 0; --row) {
      bool full = true;
      for (int col = 0; col < kBoardWidth; ++col) {
        if (board_[row * kBoardWidth + col] == 0) {
          full = false;
          break;
        }
      }
      if (!full) {
        continue;
      }

      ++cleared;
      for (int moveRow = row; moveRow > 0; --moveRow) {
        for (int col = 0; col < kBoardWidth; ++col) {
          board_[moveRow * kBoardWidth + col] = board_[(moveRow - 1) * kBoardWidth + col];
        }
      }
      for (int col = 0; col < kBoardWidth; ++col) {
        board_[col] = 0;
      }
      ++row;
    }

    if (cleared == 0) {
      return;
    }

    static constexpr int kLineScores[] = {0, 100, 300, 500, 800};
    score_ += kLineScores[cleared] * level_;
    lines_ += cleared;
    level_ = 1 + (lines_ / 10);
  }

  unsigned long dropIntervalMs() const {
    const int speedIndex = level_ - 1;
    const unsigned long interval = 1000UL - static_cast<unsigned long>(speedIndex) * 80UL;
    return interval < 200UL ? 200UL : interval;
  }

  int ghostDropRow() const {
    ActivePiece ghost = current_;
    while (true) {
      ActivePiece next = ghost;
      next.y += 1;
      if (collides(next)) {
        break;
      }
      ghost = next;
    }
    return ghost.y;
  }

  void layout(int* boardLeft, int* boardTop, int* panelLeft) const {
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    const int panelWidth = 210;
    const int totalWidth = kBoardPixelWidth + 36 + panelWidth;
    *boardLeft = (screenW - totalWidth) / 2;
    *boardTop = (screenH - kBoardPixelHeight) / 2;
    *panelLeft = *boardLeft + kBoardPixelWidth + 36;
  }

  void drawCell(int boardLeft, int boardTop, int col, int row, bool filled, bool outlineOnly = false) const {
    if (row < 0 || row >= kBoardHeight) {
      return;
    }
    const int px = boardLeft + col * kCellSize + 1;
    const int py = boardTop + row * kCellSize + 1;
    const int size = kCellSize - 2;
    if (filled) {
      renderer.rectangle.fill(px, py, size, size, kInk);
    } else if (outlineOnly) {
      renderer.rectangle.render(px, py, size, size);
    }
  }

  void drawPiece(int boardLeft, int boardTop, PieceType type, int rotation, int px, int py, bool filled,
                 bool outlineOnly = false) const {
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!shapeCell(type, rotation, row, col)) {
          continue;
        }
        drawCell(boardLeft, boardTop, px + col, py + row, filled, outlineOnly);
      }
    }
  }

  void drawPreview(int panelLeft, int y, PieceType type) const {
    const int previewSize = 16;
    const int boxW = previewSize * 4 + 12;
    const int boxH = previewSize * 4 + 12;
    renderer.rectangle.render(panelLeft, y, boxW, boxH);

    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        if (!shapeCell(type, 0, row, col)) {
          continue;
        }
        const int px = panelLeft + 6 + col * previewSize + 1;
        const int py = y + 6 + row * previewSize + 1;
        renderer.rectangle.fill(px, py, previewSize - 2, previewSize - 2, kInk);
      }
    }
  }

  void drawLabelValue(int x, int y, const char* label, const char* value) const {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, x, y, label, true, EpdFontFamily::BOLD);
    const int lineH = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_14_FONT_ID);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, x, y + lineH + 2, value);
  }

  void render(bool forceFullRefresh) {
    int boardLeft = 0;
    int boardTop = 0;
    int panelLeft = 0;
    layout(&boardLeft, &boardTop, &panelLeft);

    renderer.clearScreen();

    renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, boardLeft, boardTop - 28, "TETRIS", true,
                         EpdFontFamily::BOLD);

    renderer.rectangle.render(boardLeft - 2, boardTop - 2, kBoardPixelWidth + 4, kBoardPixelHeight + 4);

    for (int row = kHiddenRows; row < kBoardRows; ++row) {
      for (int col = 0; col < kBoardWidth; ++col) {
        if (board_[row * kBoardWidth + col] != 0) {
          drawCell(boardLeft, boardTop, col, row - kHiddenRows, true);
        }
      }
    }

    const int ghostY = ghostDropRow();
    if (ghostY != current_.y) {
      drawPiece(boardLeft, boardTop, current_.type, current_.rotation, current_.x, ghostY - kHiddenRows, false,
                true);
    }

    drawPiece(boardLeft, boardTop, current_.type, current_.rotation, current_.x, current_.y - kHiddenRows, true);

    char scoreText[16];
    char levelText[8];
    char linesText[8];
    std::snprintf(scoreText, sizeof(scoreText), "%d", score_);
    std::snprintf(levelText, sizeof(levelText), "%d", level_);
    std::snprintf(linesText, sizeof(linesText), "%d", lines_);

    int statY = boardTop + 8;
    drawLabelValue(panelLeft, statY, "Score", scoreText);
    statY += 56;
    drawLabelValue(panelLeft, statY, "Level", levelText);
    statY += 56;
    drawLabelValue(panelLeft, statY, "Lines", linesText);
    statY += 56;

    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, panelLeft, statY, "Next", true, EpdFontFamily::BOLD);
    drawPreview(panelLeft, statY + 24, next_);

    const int hintY = boardTop + kBoardPixelHeight + 12;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, boardLeft, hintY,
                         "Left/Right Move  Down Soft  Up Hard  Confirm Rotate  Back Exit");

    if (gameOver_) {
      const int overlayW = 320;
      const int overlayH = 120;
      const int overlayX = (renderer.getScreenWidth() - overlayW) / 2;
      const int overlayY = (renderer.getScreenHeight() - overlayH) / 2;
      renderer.rectangle.fill(overlayX, overlayY, overlayW, overlayH, kPaper);
      renderer.rectangle.render(overlayX, overlayY, overlayW, overlayH);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, overlayX + 78, overlayY + 18, "GAME OVER", true,
                           EpdFontFamily::BOLD);
      char finalScore[24];
      std::snprintf(finalScore, sizeof(finalScore), "Score: %d", score_);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, overlayX + 92, overlayY + 48, finalScore);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, overlayX + 48, overlayY + 82,
                           "Confirm or Back to exit");
    }

    const bool useFull = forceFullRefresh || firstRender_;
    renderer.displayBuffer(useFull ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    firstRender_ = false;
  }
};
