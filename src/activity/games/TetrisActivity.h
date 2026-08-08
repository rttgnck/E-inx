#pragma once

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>
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
    computeLayout();
    resetGame();
    dirty_ = true;
    doRender(true);
  }

  void onExit() override { Activity::onExit(); }

  void loop() override {
    using Btn = MappedInputManager::Button;

    if (mappedInput.isPressed(Btn::Back) && mappedInput.getHeldTime() >= kExitHoldMs) {
      if (!exitTriggered_) {
        exitTriggered_ = true;
        onBack_();
      }
      return;
    }
    if (mappedInput.wasReleased(Btn::Back)) {
      exitTriggered_ = false;
    }

    if (gameOver_) {
      if (mappedInput.wasPressed(Btn::Confirm) || mappedInput.wasPressed(Btn::Left) ||
          mappedInput.wasPressed(Btn::Right) || mappedInput.wasPressed(Btn::Up) ||
          mappedInput.wasPressed(Btn::Down) || mappedInput.wasPressed(Btn::PageBack) ||
          mappedInput.wasPressed(Btn::PageForward)) {
        resetGame();
        doRender(true);
      }
      return;
    }

    const unsigned long now = millis();

    if (mappedInput.wasPressed(Btn::Left)) {
      lastLeftRepeatMs_ = now;
      if (tryMove(-1, 0)) dirty_ = true;
    }
    if (mappedInput.wasPressed(Btn::Right)) {
      lastRightRepeatMs_ = now;
      if (tryMove(1, 0)) dirty_ = true;
    }
    if (mappedInput.isPressed(Btn::Left) && now - lastLeftRepeatMs_ >= kMoveRepeatMs) {
      lastLeftRepeatMs_ = now;
      if (tryMove(-1, 0)) dirty_ = true;
    }
    if (mappedInput.isPressed(Btn::Right) && now - lastRightRepeatMs_ >= kMoveRepeatMs) {
      lastRightRepeatMs_ = now;
      if (tryMove(1, 0)) dirty_ = true;
    }
    if (mappedInput.wasPressed(Btn::Back)) {
      if (tryRotate(-1)) dirty_ = true;
    }
    if (mappedInput.wasPressed(Btn::Confirm)) {
      if (tryRotate(1)) dirty_ = true;
    }
    if (mappedInput.wasPressed(Btn::Up) || mappedInput.wasPressed(Btn::PageBack)) {
      hardDrop();
      dirty_ = true;
    }
    if (mappedInput.wasPressed(Btn::Down) || mappedInput.wasPressed(Btn::PageForward)) {
      if (!holdUsed_) {
        holdPiece();
        dirty_ = true;
      }
    }

    if (now - lastDropMs_ >= dropIntervalMs()) {
      if (tryMove(0, 1)) {
        dirty_ = true;
      } else {
        lock();
        dirty_ = true;
      }
      lastDropMs_ = now;
    }

    if (dirty_ && now - lastRenderMs_ >= kMinRenderMs) {
      dirty_ = false;
      lastRenderMs_ = now;
      doRender(false);
    }
  }

  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  static constexpr int kBoardW = 10;
  static constexpr int kBoardH = 20;
  static constexpr int kHiddenRows = 2;
  static constexpr int kBoardRows = kBoardH + kHiddenRows;
  static constexpr unsigned long kExitHoldMs = 800;
  static constexpr unsigned long kMinRenderMs = 55;
  static constexpr unsigned long kMoveRepeatMs = 135;
  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);
  static constexpr int kPaper = static_cast<int>(GfxRenderer::FillTone::Paper);

  enum class Piece : uint8_t { I = 0, O, T, S, Z, L, J, Count };

  struct Active {
    Piece type = Piece::I;
    int rot = 0;
    int x = 0;
    int y = 0;
  };

  std::function<void()> onBack_;
  uint8_t board_[kBoardW * kBoardRows]{};
  Active cur_{};
  Piece next_ = Piece::I;
  Piece hold_ = Piece::Count;
  bool holdUsed_ = false;
  int score_ = 0;
  int best_ = 0;
  int level_ = 1;
  int lines_ = 0;
  bool gameOver_ = false;
  bool exitTriggered_ = false;
  bool dirty_ = false;
  bool firstRender_ = true;
  unsigned long lastDropMs_ = 0;
  unsigned long lastRenderMs_ = 0;
  unsigned long lastLeftRepeatMs_ = 0;
  unsigned long lastRightRepeatMs_ = 0;

  int cellSz_ = 32;
  int boardPixW_ = 0;
  int boardPixH_ = 0;
  int boardX_ = 0;
  int boardY_ = 0;

  static inline constexpr uint8_t kShapes[7][4][4][4] = {
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

  void computeLayout() {
    const int sw = renderer.getScreenWidth();
    const int sh = renderer.getScreenHeight();
    const int topReserve = 90;
    const int botReserve = 48;
    const int sideReserve = 45;
    const int availH = sh - topReserve - botReserve;
    const int availW = sw - sideReserve * 2;
    cellSz_ = std::min(availH / kBoardH, availW / kBoardW);
    if (cellSz_ < 12) cellSz_ = 12;
    boardPixW_ = kBoardW * cellSz_;
    boardPixH_ = kBoardH * cellSz_;
    boardX_ = (sw - boardPixW_) / 2;
    boardY_ = topReserve;
  }

  void resetGame() {
    std::memset(board_, 0, sizeof(board_));
    if (score_ > best_) best_ = score_;
    score_ = 0;
    level_ = 1;
    lines_ = 0;
    gameOver_ = false;
    exitTriggered_ = false;
    firstRender_ = true;
    hold_ = Piece::Count;
    holdUsed_ = false;
    dirty_ = true;
    next_ = rndPiece();
    spawn();
    lastDropMs_ = millis();
    lastLeftRepeatMs_ = lastDropMs_;
    lastRightRepeatMs_ = lastDropMs_;
  }

  static Piece rndPiece() { return static_cast<Piece>(random(0, 7)); }

  static bool cell(Piece t, int rot, int r, int c) {
    return kShapes[static_cast<int>(t)][rot & 3][r][c] != 0;
  }

  void spawn() {
    cur_.type = next_;
    cur_.rot = 0;
    cur_.x = 3;
    cur_.y = 0;
    next_ = rndPiece();
    holdUsed_ = false;
    if (collides(cur_)) gameOver_ = true;
  }

  bool collides(const Active& p) const {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) {
        if (!cell(p.type, p.rot, r, c)) continue;
        int bx = p.x + c, by = p.y + r;
        if (bx < 0 || bx >= kBoardW || by >= kBoardRows) return true;
        if (by >= 0 && board_[by * kBoardW + bx]) return true;
      }
    return false;
  }

  bool tryMove(int dx, int dy) {
    Active m = cur_;
    m.x += dx;
    m.y += dy;
    if (collides(m)) return false;
    cur_ = m;
    return true;
  }

  bool tryRotate(int dir) {
    Active rot = cur_;
    rot.rot = (rot.rot + dir + 4) & 3;
    if (!collides(rot)) { cur_ = rot; return true; }
    static constexpr int kicks[] = {-1, 1, -2, 2};
    for (int k : kicks) { rot.x = cur_.x + k; if (!collides(rot)) { cur_ = rot; return true; } }
    return false;
  }

  void hardDrop() {
    int n = 0;
    while (tryMove(0, 1)) ++n;
    score_ += n * 2;
    lock();
  }

  void holdPiece() {
    if (holdUsed_) return;
    holdUsed_ = true;
    if (hold_ == Piece::Count) {
      hold_ = cur_.type;
      spawn();
    } else {
      Piece tmp = hold_;
      hold_ = cur_.type;
      cur_.type = tmp;
      cur_.rot = 0;
      cur_.x = 3;
      cur_.y = 0;
      if (collides(cur_)) gameOver_ = true;
    }
  }

  void lock() {
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c) {
        if (!cell(cur_.type, cur_.rot, r, c)) continue;
        int bx = cur_.x + c, by = cur_.y + r;
        if (bx >= 0 && bx < kBoardW && by >= 0 && by < kBoardRows)
          board_[by * kBoardW + bx] = static_cast<uint8_t>(cur_.type) + 1;
      }
    clearLines();
    if (!gameOver_) spawn();
    lastDropMs_ = millis();
  }

  void clearLines() {
    int cleared = 0;
    for (int row = kBoardRows - 1; row >= 0; --row) {
      bool full = true;
      for (int c = 0; c < kBoardW; ++c)
        if (!board_[row * kBoardW + c]) { full = false; break; }
      if (!full) continue;
      ++cleared;
      for (int mr = row; mr > 0; --mr)
        for (int c = 0; c < kBoardW; ++c)
          board_[mr * kBoardW + c] = board_[(mr - 1) * kBoardW + c];
      for (int c = 0; c < kBoardW; ++c) board_[c] = 0;
      ++row;
    }
    if (!cleared) return;
    static constexpr int kScores[] = {0, 100, 300, 500, 800};
    score_ += kScores[cleared] * level_;
    lines_ += cleared;
    level_ = 1 + lines_ / 10;
  }

  unsigned long dropIntervalMs() const {
    unsigned long iv = 1000UL - static_cast<unsigned long>(level_ - 1) * 80UL;
    return iv < 200UL ? 200UL : iv;
  }

  int ghostY() const {
    Active g = cur_;
    while (true) { Active n = g; n.y++; if (collides(n)) break; g = n; }
    return g.y;
  }

  void drawCell(int col, int visRow, bool filled, bool outline = false) const {
    if (visRow < 0 || visRow >= kBoardH) return;
    int px = boardX_ + col * cellSz_ + 1;
    int py = boardY_ + visRow * cellSz_ + 1;
    int sz = cellSz_ - 2;
    if (filled) renderer.rectangle.fill(px, py, sz, sz, kInk);
    else if (outline) renderer.rectangle.render(px, py, sz, sz);
  }

  void drawPreviewBox(int x, int y, int boxSz, const char* label, Piece type) const {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, x, y - 16, label, true, EpdFontFamily::BOLD);
    renderer.rectangle.render(x, y, boxSz, boxSz);
    if (type == Piece::Count) return;
    int pcell = (boxSz - 8) / 4;
    int pad = (boxSz - pcell * 4) / 2;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        if (cell(type, 0, r, c))
          renderer.rectangle.fill(x + pad + c * pcell + 1, y + pad + r * pcell + 1,
                                  pcell - 2, pcell - 2, kInk);
  }

  void doRender(bool full) {
    const int sw = renderer.getScreenWidth();
    renderer.clearScreen();

    // --- Top-left: Score / Level+Lines / Best ---
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Score %d", score_);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, 10, 6, buf, true, EpdFontFamily::BOLD);

    std::snprintf(buf, sizeof(buf), "Level %d    Lines %d", level_, lines_);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, 10, 36, buf);

    std::snprintf(buf, sizeof(buf), "Best %d", best_);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, 10, 58, buf);

    // --- Top-right: NEXT and HOLD side by side ---
    const int boxSz = 66;
    const int boxGap = 8;
    const int holdX = sw - boxSz - 10;
    const int nextX = holdX - boxSz - boxGap;
    const int boxY = 24;
    drawPreviewBox(nextX, boxY, boxSz, "NEXT", next_);
    drawPreviewBox(holdX, boxY, boxSz, "HOLD", hold_);

    // --- Board ---
    renderer.rectangle.render(boardX_ - 2, boardY_ - 2, boardPixW_ + 4, boardPixH_ + 4);

    for (int row = kHiddenRows; row < kBoardRows; ++row)
      for (int col = 0; col < kBoardW; ++col)
        if (board_[row * kBoardW + col])
          drawCell(col, row - kHiddenRows, true);

    int gy = ghostY();
    if (gy != cur_.y)
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
          if (cell(cur_.type, cur_.rot, r, c))
            drawCell(cur_.x + c, gy + r - kHiddenRows, false, true);

    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
        if (cell(cur_.type, cur_.rot, r, c))
          drawCell(cur_.x + c, cur_.y + r - kHiddenRows, true);

    // --- Button hints ---
    renderer.ui.sideButtonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, nullptr, "Drop", "Hold");
    renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, "Rotate L", "Rotate R", "Move L", "Move R");

    // --- Game over overlay ---
    if (gameOver_) {
      if (score_ > best_) best_ = score_;
      const int oW = 300, oH = 100;
      const int oX = (sw - oW) / 2;
      const int oY = boardY_ + (boardPixH_ - oH) / 2;
      renderer.rectangle.fill(oX, oY, oW, oH, kPaper);
      renderer.rectangle.render(oX, oY, oW, oH);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, oX + 68, oY + 14, "GAME OVER", true, EpdFontFamily::BOLD);
      std::snprintf(buf, sizeof(buf), "Score: %d", score_);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, oX + 84, oY + 44, buf);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, oX + 52, oY + 74, "Press any button to restart");
    }

    renderer.displayBuffer((full || firstRender_) ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    firstRender_ = false;
  }
};
