#pragma once

/**
 * @file MazeRunnerActivity.h
 * @brief Header-only 3D first-person maze runner with minimap for the e-ink reader.
 */

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <algorithm>

#include "activity/Activity.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"

class MazeRunnerActivity final : public Activity {
 public:
  MazeRunnerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onBack)
      : Activity("Maze Runner", renderer, mappedInput), onBack_(onBack) {}

  void onEnter() override {
    Activity::onEnter();
    mazeNumber_ = 1;
    mazeSize_ = kInitialMazeSize;
    steps_ = 0;
    firstRender_ = true;
    generateMaze();
    render(true);
  }

  void onExit() override { Activity::onExit(); }

  void loop() override {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onBack_();
      return;
    }

    bool changed = false;

    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      dir_ = static_cast<uint8_t>((dir_ + 3) % 4);
      changed = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      dir_ = static_cast<uint8_t>((dir_ + 1) % 4);
      changed = true;
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      changed = tryMoveForward();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      changed = tryMoveBackward();
    }

    if (changed) {
      if (px_ == goalX_ && py_ == goalY_) {
        onGoalReached();
      }
      render(false);
    }
  }

  bool preventAutoSleep() override { return true; }

  bool skipLoopDelay() override { return true; }

 private:
  static constexpr int kInitialMazeSize = 16;
  static constexpr int kMaxMazeSize = 24;
  static constexpr int kMaxGridDim = kMaxMazeSize * 2 + 1;

  static constexpr int kViewWidth = 580;
  static constexpr int kViewHeight = 400;
  static constexpr int kViewX = 10;
  static constexpr int kViewY = 50;

  static constexpr int kMinimapSize = 150;
  static constexpr int kMinimapX = 630;
  static constexpr int kMinimapY = 50;

  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);
  static constexpr int kPaper = static_cast<int>(GfxRenderer::FillTone::Paper);

  static constexpr uint8_t kWallNorth = 0x01;
  static constexpr uint8_t kWallEast = 0x02;
  static constexpr uint8_t kWallSouth = 0x04;
  static constexpr uint8_t kWallWest = 0x08;
  static constexpr uint8_t kWallAll = kWallNorth | kWallEast | kWallSouth | kWallWest;

  static constexpr float kPi = 3.14159265f;
  static constexpr float kFov = kPi / 3.0f;

  std::function<void()> onBack_;

  int mazeSize_ = kInitialMazeSize;
  int mazeNumber_ = 1;
  int steps_ = 0;
  int px_ = 0;
  int py_ = 0;
  int goalX_ = 0;
  int goalY_ = 0;
  uint8_t dir_ = 1;  // 0=N, 1=E, 2=S, 3=W

  int gridWidth_ = 0;
  int gridHeight_ = 0;

  uint8_t cellWalls_[kMaxMazeSize * kMaxMazeSize]{};
  uint8_t wallGrid_[kMaxGridDim * kMaxGridDim]{};
  bool explored_[kMaxMazeSize * kMaxMazeSize]{};
  bool firstRender_ = true;

  bool hasWall(int x, int y, uint8_t wallBit) const {
    if (x < 0 || y < 0 || x >= mazeSize_ || y >= mazeSize_) {
      return true;
    }
    return (cellWalls_[y * mazeSize_ + x] & wallBit) != 0;
  }

  void removeWallBetween(int x1, int y1, int x2, int y2) {
    if (x1 == x2) {
      if (y1 < y2) {
        cellWalls_[y1 * mazeSize_ + x1] &= ~kWallSouth;
        cellWalls_[y2 * mazeSize_ + x2] &= ~kWallNorth;
      } else {
        cellWalls_[y2 * mazeSize_ + x2] &= ~kWallSouth;
        cellWalls_[y1 * mazeSize_ + x1] &= ~kWallNorth;
      }
      return;
    }

    if (x1 < x2) {
      cellWalls_[y1 * mazeSize_ + x1] &= ~kWallEast;
      cellWalls_[y2 * mazeSize_ + x2] &= ~kWallWest;
    } else {
      cellWalls_[y2 * mazeSize_ + x2] &= ~kWallEast;
      cellWalls_[y1 * mazeSize_ + x1] &= ~kWallWest;
    }
  }

  void buildWallGrid() {
    gridWidth_ = mazeSize_ * 2 + 1;
    gridHeight_ = mazeSize_ * 2 + 1;
    std::memset(wallGrid_, 1, static_cast<size_t>(gridWidth_ * gridHeight_));

    for (int y = 0; y < mazeSize_; ++y) {
      for (int x = 0; x < mazeSize_; ++x) {
        const int gx = x * 2 + 1;
        const int gy = y * 2 + 1;
        wallGrid_[gy * gridWidth_ + gx] = 0;

        if (!hasWall(x, y, kWallNorth)) {
          wallGrid_[(gy - 1) * gridWidth_ + gx] = 0;
        }
        if (!hasWall(x, y, kWallEast)) {
          wallGrid_[gy * gridWidth_ + gx + 1] = 0;
        }
        if (!hasWall(x, y, kWallSouth)) {
          wallGrid_[(gy + 1) * gridWidth_ + gx] = 0;
        }
        if (!hasWall(x, y, kWallWest)) {
          wallGrid_[gy * gridWidth_ + gx - 1] = 0;
        }
      }
    }
  }

  void generateMaze() {
    std::memset(cellWalls_, kWallAll, sizeof(cellWalls_));
    std::memset(explored_, 0, sizeof(explored_));

    bool visited[kMaxMazeSize * kMaxMazeSize]{};
    int stackX[kMaxMazeSize * kMaxMazeSize]{};
    int stackY[kMaxMazeSize * kMaxMazeSize]{};
    int stackTop = 0;

    const int startX = 0;
    const int startY = 0;
    stackX[stackTop] = startX;
    stackY[stackTop] = startY;
    ++stackTop;
    visited[startY * mazeSize_ + startX] = true;

    randomSeed(static_cast<uint32_t>(millis() ^ (mazeNumber_ * 7919)));

    while (stackTop > 0) {
      const int cx = stackX[stackTop - 1];
      const int cy = stackY[stackTop - 1];

      int neighbors[4][2];
      int neighborCount = 0;

      const int offsets[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
      for (int i = 0; i < 4; ++i) {
        const int nx = cx + offsets[i][0];
        const int ny = cy + offsets[i][1];
        if (nx >= 0 && ny >= 0 && nx < mazeSize_ && ny < mazeSize_ && !visited[ny * mazeSize_ + nx]) {
          neighbors[neighborCount][0] = nx;
          neighbors[neighborCount][1] = ny;
          ++neighborCount;
        }
      }

      if (neighborCount == 0) {
        --stackTop;
        continue;
      }

      const int pick = random(neighborCount);
      const int nx = neighbors[pick][0];
      const int ny = neighbors[pick][1];
      removeWallBetween(cx, cy, nx, ny);
      visited[ny * mazeSize_ + nx] = true;
      stackX[stackTop] = nx;
      stackY[stackTop] = ny;
      ++stackTop;
    }

    px_ = startX;
    py_ = startY;
    dir_ = 1;
    goalX_ = mazeSize_ - 1;
    goalY_ = mazeSize_ - 1;

    buildWallGrid();
    revealArea(px_, py_);
  }

  void revealArea(int x, int y) {
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        const int nx = x + dx;
        const int ny = y + dy;
        if (nx >= 0 && ny >= 0 && nx < mazeSize_ && ny < mazeSize_) {
          explored_[ny * mazeSize_ + nx] = true;
        }
      }
    }
  }

  bool canMoveFrom(int x, int y, uint8_t direction) const {
    switch (direction) {
      case 0:
        return !hasWall(x, y, kWallNorth);
      case 1:
        return !hasWall(x, y, kWallEast);
      case 2:
        return !hasWall(x, y, kWallSouth);
      case 3:
        return !hasWall(x, y, kWallWest);
      default:
        return false;
    }
  }

  bool tryMoveForward() {
    if (!canMoveFrom(px_, py_, dir_)) {
      return false;
    }

    switch (dir_) {
      case 0:
        --py_;
        break;
      case 1:
        ++px_;
        break;
      case 2:
        ++py_;
        break;
      case 3:
        --px_;
        break;
      default:
        break;
    }

    ++steps_;
    revealArea(px_, py_);
    return true;
  }

  bool tryMoveBackward() {
    const uint8_t backDir = static_cast<uint8_t>((dir_ + 2) % 4);
    if (!canMoveFrom(px_, py_, backDir)) {
      return false;
    }

    switch (backDir) {
      case 0:
        --py_;
        break;
      case 1:
        ++px_;
        break;
      case 2:
        ++py_;
        break;
      case 3:
        --px_;
        break;
      default:
        break;
    }

    ++steps_;
    revealArea(px_, py_);
    return true;
  }

  void onGoalReached() {
    ++mazeNumber_;
    if (mazeSize_ < kMaxMazeSize) {
      ++mazeSize_;
    }
    steps_ = 0;
    generateMaze();
  }

  float castRay(float posX, float posY, float rayDirX, float rayDirY) const {
    int mapX = static_cast<int>(posX);
    int mapY = static_cast<int>(posY);

    const float deltaDistX = (rayDirX == 0.0f) ? 1e30f : std::fabs(1.0f / rayDirX);
    const float deltaDistY = (rayDirY == 0.0f) ? 1e30f : std::fabs(1.0f / rayDirY);

    int stepX = 0;
    int stepY = 0;
    float sideDistX = 0.0f;
    float sideDistY = 0.0f;

    if (rayDirX < 0.0f) {
      stepX = -1;
      sideDistX = (posX - static_cast<float>(mapX)) * deltaDistX;
    } else {
      stepX = 1;
      sideDistX = (static_cast<float>(mapX) + 1.0f - posX) * deltaDistX;
    }

    if (rayDirY < 0.0f) {
      stepY = -1;
      sideDistY = (posY - static_cast<float>(mapY)) * deltaDistY;
    } else {
      stepY = 1;
      sideDistY = (static_cast<float>(mapY) + 1.0f - posY) * deltaDistY;
    }

    bool hit = false;
    float perpWallDist = 20.0f;
    int side = 0;

    for (int guard = 0; guard < 128 && !hit; ++guard) {
      if (sideDistX < sideDistY) {
        sideDistX += deltaDistX;
        mapX += stepX;
        side = 0;
      } else {
        sideDistY += deltaDistY;
        mapY += stepY;
        side = 1;
      }

      if (mapX < 0 || mapY < 0 || mapX >= gridWidth_ || mapY >= gridHeight_) {
        perpWallDist = 20.0f;
        break;
      }

      if (wallGrid_[mapY * gridWidth_ + mapX] != 0) {
        hit = true;
        if (side == 0) {
          perpWallDist = (static_cast<float>(mapX) - posX + (1 - stepX) / 2.0f) / rayDirX;
        } else {
          perpWallDist = (static_cast<float>(mapY) - posY + (1 - stepY) / 2.0f) / rayDirY;
        }
      }
    }

    if (perpWallDist < 0.05f) {
      perpWallDist = 0.05f;
    }

    return perpWallDist;
  }

  void render3DView() const {
    const float posX = static_cast<float>(px_ * 2 + 1);
    const float posY = static_cast<float>(py_ * 2 + 1);
    const float baseAngle = static_cast<float>(dir_) * (kPi / 2.0f) - (kPi / 2.0f);

    renderer.rectangle.fill(kViewX, kViewY, kViewWidth, kViewHeight, kPaper);
    renderer.rectangle.render(kViewX, kViewY, kViewWidth, kViewHeight);

    for (int col = 0; col < kViewWidth; ++col) {
      const float cameraX = (2.0f * static_cast<float>(col) / static_cast<float>(kViewWidth)) - 1.0f;
      const float rayAngle = baseAngle + std::atan(cameraX * std::tan(kFov / 2.0f));
      const float rayDirX = std::cos(rayAngle);
      const float rayDirY = std::sin(rayAngle);

      const float dist = castRay(posX, posY, rayDirX, rayDirY);
      int stripeHeight = static_cast<int>(static_cast<float>(kViewHeight) / (dist * 0.65f));
      if (stripeHeight > kViewHeight) {
        stripeHeight = kViewHeight;
      }
      if (stripeHeight < 1) {
        stripeHeight = 1;
      }

      const int drawY = kViewY + (kViewHeight - stripeHeight) / 2;
      renderer.rectangle.fill(kViewX + col, drawY, 1, stripeHeight, kInk);
    }
  }

  void renderMinimap() const {
    renderer.rectangle.fill(kMinimapX, kMinimapY, kMinimapSize, kMinimapSize, kPaper);
    renderer.rectangle.render(kMinimapX, kMinimapY, kMinimapSize, kMinimapSize);

    const int cellPx = kMinimapSize / mazeSize_;
    if (cellPx < 2) {
      return;
    }

    for (int y = 0; y < mazeSize_; ++y) {
      for (int x = 0; x < mazeSize_; ++x) {
        if (!explored_[y * mazeSize_ + x]) {
          continue;
        }

        const int cx = kMinimapX + x * cellPx;
        const int cy = kMinimapY + y * cellPx;

        if (hasWall(x, y, kWallNorth)) {
          renderer.line.render(cx, cy, cx + cellPx, cy);
        }
        if (hasWall(x, y, kWallEast)) {
          renderer.line.render(cx + cellPx, cy, cx + cellPx, cy + cellPx);
        }
        if (hasWall(x, y, kWallSouth)) {
          renderer.line.render(cx, cy + cellPx, cx + cellPx, cy + cellPx);
        }
        if (hasWall(x, y, kWallWest)) {
          renderer.line.render(cx, cy, cx, cy + cellPx);
        }
      }
    }

    const int goalCenterX = kMinimapX + goalX_ * cellPx + cellPx / 2;
    const int goalCenterY = kMinimapY + goalY_ * cellPx + cellPx / 2;
    const int goalMark = std::max(3, cellPx / 3);
    renderer.rectangle.render(goalCenterX - goalMark, goalCenterY - goalMark, goalMark * 2, goalMark * 2);

    const int playerCenterX = kMinimapX + px_ * cellPx + cellPx / 2;
    const int playerCenterY = kMinimapY + py_ * cellPx + cellPx / 2;
    renderer.rectangle.fill(playerCenterX - 2, playerCenterY - 2, 4, 4, kInk);

    int arrowX = playerCenterX;
    int arrowY = playerCenterY;
    switch (dir_) {
      case 0:
        arrowY -= cellPx / 2;
        break;
      case 1:
        arrowX += cellPx / 2;
        break;
      case 2:
        arrowY += cellPx / 2;
        break;
      case 3:
        arrowX -= cellPx / 2;
        break;
      default:
        break;
    }
    renderer.line.render(playerCenterX, playerCenterY, arrowX, arrowY);
  }

  void renderHud() const {
    char mazeText[24];
    char stepText[24];
    std::snprintf(mazeText, sizeof(mazeText), "Maze %d", mazeNumber_);
    std::snprintf(stepText, sizeof(stepText), "Steps: %d", steps_);

    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, kMinimapX, kMinimapY + kMinimapSize + 16, mazeText);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, kMinimapX, kMinimapY + kMinimapSize + 38, stepText);

    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, kViewX, 18, "MAZE RUNNER", true, EpdFontFamily::BOLD);

    const int hintY = kViewY + kViewHeight + 12;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, kViewX, hintY,
                         "Up/Down Move  Left/Right Turn  Back Exit");
  }

  void render(bool forceFullRefresh) {
    renderer.clearScreen();

    render3DView();
    renderMinimap();
    renderHud();

    const bool useFull = forceFullRefresh || firstRender_;
    renderer.displayBuffer(useFull ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    firstRender_ = false;
  }
};
