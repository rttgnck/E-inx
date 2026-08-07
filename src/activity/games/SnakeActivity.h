#pragma once

/**
 * @file SnakeActivity.h
 * @brief Classic Snake game for the 800x480 e-ink display.
 */

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <cstdio>
#include <functional>

#include "activity/Activity.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"

class SnakeActivity final : public Activity {
 public:
  explicit SnakeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                         const std::function<void()>& onBack)
      : Activity("Snake", renderer, mappedInput), onBack_(onBack) {}

  void onEnter() override {
    randomSeed(millis());
    resetGame();
    render();
  }

  void loop() override {
    using Button = MappedInputManager::Button;

    if (mappedInput.wasPressed(Button::Back)) {
      onBack_();
      return;
    }

    if (gameOver_) {
      if (mappedInput.wasPressed(Button::Confirm) || mappedInput.wasPressed(Button::Up) ||
          mappedInput.wasPressed(Button::Down) || mappedInput.wasPressed(Button::Left) ||
          mappedInput.wasPressed(Button::Right)) {
        resetGame();
        render();
      }
      return;
    }

    handleDirectionInput();

    const unsigned long now = millis();
    if (now - lastTickMs_ < tickIntervalMs()) {
      return;
    }
    lastTickMs_ = now;

    if (!step()) {
      gameOver_ = true;
    }

    render();
  }

  bool preventAutoSleep() override { return true; }

  bool skipLoopDelay() override { return true; }

 private:
  enum class Direction : uint8_t { Up, Down, Left, Right };

  struct Point {
    int16_t x;
    int16_t y;
  };

  static constexpr int kCellSize = 18;
  static constexpr int kScoreBarHeight = 40;
  static constexpr int kMaxSnakeLen = 1280;
  static constexpr unsigned long kBaseTickMs = 400;
  static constexpr unsigned long kMinTickMs = 150;
  static constexpr int kInitialSnakeLen = 3;

  const std::function<void()> onBack_;
  Point snake_[kMaxSnakeLen]{};
  int snakeLen_ = kInitialSnakeLen;
  int foodX_ = 0;
  int foodY_ = 0;
  int score_ = 0;
  int gridCols_ = 0;
  int gridRows_ = 0;
  int gridOffsetX_ = 0;
  int gridOffsetY_ = 0;
  int playWidth_ = 0;
  int playHeight_ = 0;
  Direction direction_ = Direction::Right;
  Direction nextDirection_ = Direction::Right;
  bool gameOver_ = false;
  unsigned long lastTickMs_ = 0;

  void resetGame() {
    computeGrid();

    const int startX = gridCols_ / 2;
    const int startY = gridRows_ / 2;
    snakeLen_ = kInitialSnakeLen;
    for (int i = 0; i < snakeLen_; ++i) {
      snake_[i] = {static_cast<int16_t>(startX - i), static_cast<int16_t>(startY)};
    }

    direction_ = Direction::Right;
    nextDirection_ = Direction::Right;
    score_ = 0;
    gameOver_ = false;
    lastTickMs_ = millis();
    spawnFood();
  }

  void computeGrid() {
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    const int playAreaH = screenH - kScoreBarHeight;

    gridCols_ = screenW / kCellSize;
    gridRows_ = playAreaH / kCellSize;
    playWidth_ = gridCols_ * kCellSize;
    playHeight_ = gridRows_ * kCellSize;
    gridOffsetX_ = (screenW - playWidth_) / 2;
    gridOffsetY_ = kScoreBarHeight + (playAreaH - playHeight_) / 2;
  }

  static bool isOpposite(Direction current, Direction next) {
    return (current == Direction::Up && next == Direction::Down) ||
           (current == Direction::Down && next == Direction::Up) ||
           (current == Direction::Left && next == Direction::Right) ||
           (current == Direction::Right && next == Direction::Left);
  }

  void queueDirection(Direction dir) {
    if (!isOpposite(direction_, dir)) {
      nextDirection_ = dir;
    }
  }

  void handleDirectionInput() {
    using Button = MappedInputManager::Button;

    if (mappedInput.wasPressed(Button::Up)) {
      queueDirection(Direction::Up);
    } else if (mappedInput.wasPressed(Button::Down)) {
      queueDirection(Direction::Down);
    } else if (mappedInput.wasPressed(Button::Left)) {
      queueDirection(Direction::Left);
    } else if (mappedInput.wasPressed(Button::Right)) {
      queueDirection(Direction::Right);
    }
  }

  unsigned long tickIntervalMs() const {
    const int growth = snakeLen_ - kInitialSnakeLen;
    const unsigned long interval = kBaseTickMs - static_cast<unsigned long>(growth) * 8UL;
    return interval < kMinTickMs ? kMinTickMs : interval;
  }

  bool isOccupied(int x, int y) const {
    for (int i = 0; i < snakeLen_; ++i) {
      if (snake_[i].x == x && snake_[i].y == y) {
        return true;
      }
    }
    return false;
  }

  void spawnFood() {
    if (snakeLen_ >= gridCols_ * gridRows_) {
      return;
    }

    int attempts = 0;
    do {
      foodX_ = random(0, gridCols_);
      foodY_ = random(0, gridRows_);
      ++attempts;
    } while (isOccupied(foodX_, foodY_) && attempts < 1000);
  }

  bool step() {
    direction_ = nextDirection_;

    Point newHead = snake_[0];
    switch (direction_) {
      case Direction::Up:
        --newHead.y;
        break;
      case Direction::Down:
        ++newHead.y;
        break;
      case Direction::Left:
        --newHead.x;
        break;
      case Direction::Right:
        ++newHead.x;
        break;
    }

    if (newHead.x < 0 || newHead.x >= gridCols_ || newHead.y < 0 || newHead.y >= gridRows_) {
      return false;
    }

    for (int i = 0; i < snakeLen_; ++i) {
      if (snake_[i].x == newHead.x && snake_[i].y == newHead.y) {
        return false;
      }
    }

    const bool ateFood = newHead.x == foodX_ && newHead.y == foodY_;

    if (ateFood) {
      if (snakeLen_ >= kMaxSnakeLen) {
        return false;
      }
      for (int i = snakeLen_; i > 0; --i) {
        snake_[i] = snake_[i - 1];
      }
      snake_[0] = newHead;
      ++snakeLen_;
      ++score_;
      spawnFood();
    } else {
      for (int i = snakeLen_ - 1; i > 0; --i) {
        snake_[i] = snake_[i - 1];
      }
      snake_[0] = newHead;
    }

    return true;
  }

  void drawCell(int gridX, int gridY, int inset = 1) const {
    const int x = gridOffsetX_ + gridX * kCellSize + inset;
    const int y = gridOffsetY_ + gridY * kCellSize + inset;
    const int size = kCellSize - inset * 2;
    renderer.rectangle.fill(x, y, size, size, static_cast<int>(GfxRenderer::FillTone::Ink));
  }

  void render() const {
    renderer.clearScreen();

    char scoreText[32];
    snprintf(scoreText, sizeof(scoreText), "Snake  Score: %d", score_);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, gridOffsetX_, 12, scoreText);

    renderer.rectangle.render(gridOffsetX_, gridOffsetY_, playWidth_, playHeight_);

    drawCell(foodX_, foodY_, 3);

    for (int i = 0; i < snakeLen_; ++i) {
      drawCell(snake_[i].x, snake_[i].y, i == 0 ? 0 : 1);
    }

    if (gameOver_) {
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_16_FONT_ID, gridOffsetY_ + playHeight_ / 2 - 24, "Game Over",
                             true, EpdFontFamily::BOLD);
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, gridOffsetY_ + playHeight_ / 2 + 4,
                             "Press any direction to restart");
    }

    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
};
