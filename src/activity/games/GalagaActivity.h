#pragma once

/**
 * @file GalagaActivity.h
 * @brief Turn-based Galaga-style space shooter for the 800x480 e-ink display.
 */

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <cstdio>
#include <functional>

#include "activity/Activity.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"

class GalagaActivity final : public Activity {
 public:
  GalagaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onBack)
      : Activity("Galaga", renderer, mappedInput), onBack_(onBack) {}

  void onEnter() override {
    randomSeed(millis());
    resetGame();
    render(true);
  }

  void loop() override {
    using Button = MappedInputManager::Button;

    if (mappedInput.isPressed(Button::Back) && mappedInput.getHeldTime() >= kExitHoldMs) {
      if (!exitTriggered_) {
        exitTriggered_ = true;
        onBack_();
      }
      return;
    }
    if (mappedInput.wasReleased(Button::Back)) {
      exitTriggered_ = false;
    }

    if (gameOver_) {
      if (mappedInput.wasPressed(Button::Confirm) || mappedInput.wasPressed(Button::Up)) {
        resetGame();
        render(true);
      }
      return;
    }

    const unsigned long now = millis();
    const bool inputPressed = mappedInput.wasPressed(Button::Left) || mappedInput.wasPressed(Button::Right) ||
                              mappedInput.wasPressed(Button::Confirm) || mappedInput.wasPressed(Button::Up) ||
                              mappedInput.wasPressed(Button::Down);

    if (!inputPressed && now - lastTurnMs_ < turnIntervalMs()) {
      return;
    }
    lastTurnMs_ = now;

    handleInput();
    advanceTurn();
    render(false);
  }

  bool preventAutoSleep() override { return true; }

  bool skipLoopDelay() override { return true; }

 private:
  struct Bullet {
    int16_t x = 0;
    int16_t y = 0;
    bool active = false;
  };

  struct Enemy {
    int16_t x = 0;
    int16_t y = 0;
    bool alive = false;
    uint8_t type = 0;
  };

  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);
  static constexpr int kHudHeight = 40;
  static constexpr int kPlayMarginX = 24;
  static constexpr int kPlayMarginBottom = 16;

  static constexpr int kPlayerW = 28;
  static constexpr int kPlayerH = 20;
  static constexpr int kEnemyW = 24;
  static constexpr int kEnemyH = 18;
  static constexpr int kEnemyGapX = 10;
  static constexpr int kEnemyGapY = 10;
  static constexpr int kBulletW = 4;
  static constexpr int kBulletH = 12;
  static constexpr int kPlayerStep = 20;
  static constexpr int kBulletStep = 24;
  static constexpr int kFormationDrop = 14;

  static constexpr int kMaxEnemies = 48;
  static constexpr int kMaxEnemyBullets = 8;
  static constexpr unsigned long kBaseTurnMs = 450;
  static constexpr unsigned long kMinTurnMs = 220;
  static constexpr unsigned long kExitHoldMs = 800;

  std::function<void()> onBack_;
  Enemy enemies_[kMaxEnemies]{};
  int enemyCount_ = 0;
  Bullet playerBullet_{};
  Bullet enemyBullets_[kMaxEnemyBullets]{};

  int playX_ = 0;
  int playY_ = 0;
  int playW_ = 0;
  int playH_ = 0;

  int playerX_ = 0;
  int playerY_ = 0;
  int formationDir_ = 1;
  int score_ = 0;
  int lives_ = 3;
  int wave_ = 1;
  int turnsSinceEnemyMove_ = 0;
  int turnsSinceEnemyFire_ = 0;
  int enemyMoveEvery_ = 2;
  int enemyFireEvery_ = 5;
  bool gameOver_ = false;
  bool exitTriggered_ = false;
  bool firstRender_ = true;
  unsigned long lastTurnMs_ = 0;

  void computePlayArea() {
    const int screenW = renderer.getScreenWidth();
    const int screenH = renderer.getScreenHeight();
    playW_ = screenW - kPlayMarginX * 2;
    playH_ = screenH - kHudHeight - kPlayMarginBottom;
    playX_ = (screenW - playW_) / 2;
    playY_ = kHudHeight;
  }

  unsigned long turnIntervalMs() const {
    const int speedup = (wave_ - 1) * 18;
    const unsigned long interval = kBaseTurnMs - static_cast<unsigned long>(speedup);
    return interval < kMinTurnMs ? kMinTurnMs : interval;
  }

  void resetGame() {
    computePlayArea();
    score_ = 0;
    lives_ = 3;
    wave_ = 1;
    gameOver_ = false;
    firstRender_ = true;
    playerBullet_.active = false;
    for (auto& bullet : enemyBullets_) {
      bullet.active = false;
    }
    resetPlayer();
    spawnWave();
    lastTurnMs_ = millis();
  }

  void resetPlayer() {
    playerX_ = playX_ + playW_ / 2 - kPlayerW / 2;
    playerY_ = playY_ + playH_ - kPlayerH - 8;
  }

  void spawnWave() {
    enemyCount_ = 0;
    formationDir_ = 1;
    turnsSinceEnemyMove_ = 0;
    turnsSinceEnemyFire_ = 0;
    enemyMoveEvery_ = wave_ <= 2 ? 2 : (wave_ <= 5 ? 1 : 1);
    enemyFireEvery_ = wave_ <= 2 ? 6 : (wave_ <= 5 ? 4 : 3);

    const int rows = 2 + (wave_ - 1) / 2;
    const int cols = 4 + (wave_ - 1) / 3;
    const int clampedRows = rows > 6 ? 6 : rows;
    const int clampedCols = cols > 8 ? 8 : cols;

    const int formationW = clampedCols * kEnemyW + (clampedCols - 1) * kEnemyGapX;
    const int startX = playX_ + (playW_ - formationW) / 2;
    const int startY = playY_ + 24;

    for (int row = 0; row < clampedRows; ++row) {
      for (int col = 0; col < clampedCols; ++col) {
        if (enemyCount_ >= kMaxEnemies) {
          return;
        }
        Enemy& enemy = enemies_[enemyCount_++];
        enemy.alive = true;
        enemy.type = static_cast<uint8_t>((row + col) % 3);
        enemy.x = static_cast<int16_t>(startX + col * (kEnemyW + kEnemyGapX));
        enemy.y = static_cast<int16_t>(startY + row * (kEnemyH + kEnemyGapY));
      }
    }
  }

  void handleInput() {
    using Button = MappedInputManager::Button;

    if (mappedInput.wasPressed(Button::Left)) {
      playerX_ -= kPlayerStep;
      if (playerX_ < playX_ + 4) {
        playerX_ = playX_ + 4;
      }
    } else if (mappedInput.wasPressed(Button::Right)) {
      playerX_ += kPlayerStep;
      if (playerX_ + kPlayerW > playX_ + playW_ - 4) {
        playerX_ = playX_ + playW_ - kPlayerW - 4;
      }
    }

    if (mappedInput.wasPressed(Button::Confirm) && !playerBullet_.active) {
      playerBullet_.active = true;
      playerBullet_.x = static_cast<int16_t>(playerX_ + kPlayerW / 2 - kBulletW / 2);
      playerBullet_.y = static_cast<int16_t>(playerY_ - kBulletH);
    }
  }

  void advanceTurn() {
    movePlayerBullet();
    moveEnemyBullets();
    moveEnemies();
    maybeEnemyFire();
    resolveCollisions();
    checkWaveClear();
    checkPlayerDeath();
  }

  void movePlayerBullet() {
    if (!playerBullet_.active) {
      return;
    }
    playerBullet_.y = static_cast<int16_t>(playerBullet_.y - kBulletStep);
    if (playerBullet_.y + kBulletH < playY_) {
      playerBullet_.active = false;
    }
  }

  void moveEnemyBullets() {
    for (auto& bullet : enemyBullets_) {
      if (!bullet.active) {
        continue;
      }
      bullet.y = static_cast<int16_t>(bullet.y + kBulletStep);
      if (bullet.y > playY_ + playH_) {
        bullet.active = false;
      }
    }
  }

  void moveEnemies() {
    ++turnsSinceEnemyMove_;
    if (turnsSinceEnemyMove_ < enemyMoveEvery_) {
      return;
    }
    turnsSinceEnemyMove_ = 0;

    int minX = playX_ + playW_;
    int maxX = playX_;
    for (int i = 0; i < enemyCount_; ++i) {
      if (!enemies_[i].alive) {
        continue;
      }
      minX = enemies_[i].x < minX ? enemies_[i].x : minX;
      maxX = enemies_[i].x + kEnemyW > maxX ? enemies_[i].x + kEnemyW : maxX;
    }

    bool hitEdge = false;
    if (formationDir_ > 0 && maxX + kPlayerStep >= playX_ + playW_ - 4) {
      hitEdge = true;
    } else if (formationDir_ < 0 && minX - kPlayerStep <= playX_ + 4) {
      hitEdge = true;
    }

    if (hitEdge) {
      formationDir_ = -formationDir_;
      for (int i = 0; i < enemyCount_; ++i) {
        if (enemies_[i].alive) {
          enemies_[i].y = static_cast<int16_t>(enemies_[i].y + kFormationDrop);
        }
      }
    } else {
      for (int i = 0; i < enemyCount_; ++i) {
        if (enemies_[i].alive) {
          enemies_[i].x = static_cast<int16_t>(enemies_[i].x + formationDir_ * kPlayerStep);
        }
      }
    }
  }

  void maybeEnemyFire() {
    ++turnsSinceEnemyFire_;
    if (turnsSinceEnemyFire_ < enemyFireEvery_) {
      return;
    }
    turnsSinceEnemyFire_ = 0;

    int candidates[kMaxEnemies];
    int candidateCount = 0;
    for (int i = 0; i < enemyCount_; ++i) {
      if (!enemies_[i].alive) {
        continue;
      }
      bool bottomMost = true;
      for (int j = 0; j < enemyCount_; ++j) {
        if (i == j || !enemies_[j].alive) {
          continue;
        }
        if (abs(enemies_[j].x - enemies_[i].x) < kEnemyW &&
            enemies_[j].y > enemies_[i].y) {
          bottomMost = false;
          break;
        }
      }
      if (bottomMost) {
        candidates[candidateCount++] = i;
      }
    }

    if (candidateCount == 0) {
      return;
    }

    const int pick = random(0, candidateCount);
    const Enemy& shooter = enemies_[candidates[pick]];

    for (auto& bullet : enemyBullets_) {
      if (!bullet.active) {
        bullet.active = true;
        bullet.x = static_cast<int16_t>(shooter.x + kEnemyW / 2 - kBulletW / 2);
        bullet.y = static_cast<int16_t>(shooter.y + kEnemyH);
        break;
      }
    }
  }

  static bool overlaps(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
  }

  void resolveCollisions() {
    if (playerBullet_.active) {
      for (int i = 0; i < enemyCount_; ++i) {
        Enemy& enemy = enemies_[i];
        if (!enemy.alive) {
          continue;
        }
        if (overlaps(playerBullet_.x, playerBullet_.y, kBulletW, kBulletH, enemy.x, enemy.y, kEnemyW, kEnemyH)) {
          enemy.alive = false;
          playerBullet_.active = false;
          score_ += 100;
          break;
        }
      }
    }

    for (auto& bullet : enemyBullets_) {
      if (!bullet.active) {
        continue;
      }
      if (overlaps(bullet.x, bullet.y, kBulletW, kBulletH, playerX_, playerY_, kPlayerW, kPlayerH)) {
        bullet.active = false;
        loseLife();
        break;
      }
    }
  }

  void checkWaveClear() {
    for (int i = 0; i < enemyCount_; ++i) {
      if (enemies_[i].alive) {
        return;
      }
    }

    score_ += wave_ * 500;
    ++wave_;
    playerBullet_.active = false;
    for (auto& bullet : enemyBullets_) {
      bullet.active = false;
    }
    resetPlayer();
    spawnWave();
  }

  void checkPlayerDeath() {
    for (int i = 0; i < enemyCount_; ++i) {
      if (!enemies_[i].alive) {
        continue;
      }
      if (overlaps(playerX_, playerY_, kPlayerW, kPlayerH, enemies_[i].x, enemies_[i].y, kEnemyW, kEnemyH)) {
        loseLife();
        return;
      }
      if (enemies_[i].y + kEnemyH >= playerY_) {
        loseLife();
        return;
      }
    }
  }

  void loseLife() {
    --lives_;
    playerBullet_.active = false;
    for (auto& bullet : enemyBullets_) {
      bullet.active = false;
    }
    if (lives_ <= 0) {
      gameOver_ = true;
      return;
    }
    resetPlayer();
  }

  void fillInk(int x, int y, int w, int h) const { renderer.rectangle.fill(x, y, w, h, kInk); }

  void drawPlayerShip() const {
    const int cx = playerX_ + kPlayerW / 2;
    fillInk(cx - 4, playerY_, 8, 4);
    fillInk(cx - 8, playerY_ + 4, 16, 4);
    fillInk(cx - 12, playerY_ + 8, 24, 4);
    fillInk(cx - 4, playerY_ + 12, 8, 8);
  }

  void drawEnemyType0(int x, int y) const {
    fillInk(x + 4, y, 8, 4);
    fillInk(x, y + 4, 24, 4);
    fillInk(x + 2, y + 8, 20, 4);
    fillInk(x, y + 12, 8, 4);
    fillInk(x + 16, y + 12, 8, 4);
  }

  void drawEnemyType1(int x, int y) const {
    fillInk(x + 8, y, 8, 4);
    fillInk(x + 2, y + 4, 20, 4);
    fillInk(x, y + 8, 24, 4);
    fillInk(x + 4, y + 12, 6, 4);
    fillInk(x + 14, y + 12, 6, 4);
  }

  void drawEnemyType2(int x, int y) const {
    fillInk(x, y, 6, 4);
    fillInk(x + 18, y, 6, 4);
    fillInk(x + 4, y + 4, 16, 4);
    fillInk(x, y + 8, 24, 4);
    fillInk(x + 2, y + 12, 8, 4);
    fillInk(x + 14, y + 12, 8, 4);
  }

  void drawEnemy(const Enemy& enemy) const {
    switch (enemy.type % 3) {
      case 0:
        drawEnemyType0(enemy.x, enemy.y);
        break;
      case 1:
        drawEnemyType1(enemy.x, enemy.y);
        break;
      default:
        drawEnemyType2(enemy.x, enemy.y);
        break;
    }
  }

  void drawBullet(const Bullet& bullet) const {
    if (bullet.active) {
      fillInk(bullet.x, bullet.y, kBulletW, kBulletH);
    }
  }

  void drawLives(int x, int y) const {
    for (int i = 0; i < lives_; ++i) {
      const int sx = x + i * 14;
      fillInk(sx + 4, y, 6, 3);
      fillInk(sx + 2, y + 3, 10, 3);
      fillInk(sx, y + 6, 14, 3);
    }
  }

  void render(bool fullRefresh) {
    renderer.clearScreen();

    char hud[64];
    snprintf(hud, sizeof(hud), "Galaga   Score: %d   Wave: %d", score_, wave_);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, playX_, 10, hud);

    const int livesX = playX_ + playW_ - 70;
    drawLives(livesX, 12);

    renderer.rectangle.render(playX_, playY_, playW_, playH_);

    for (int i = 0; i < enemyCount_; ++i) {
      if (enemies_[i].alive) {
        drawEnemy(enemies_[i]);
      }
    }

    drawPlayerShip();
    drawBullet(playerBullet_);
    for (const auto& bullet : enemyBullets_) {
      drawBullet(bullet);
    }

    if (gameOver_) {
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_16_FONT_ID, playY_ + playH_ / 2 - 28, "Game Over", true,
                             EpdFontFamily::BOLD);
      char finalScore[32];
      snprintf(finalScore, sizeof(finalScore), "Final score: %d", score_);
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, playY_ + playH_ / 2, finalScore);
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, playY_ + playH_ / 2 + 28,
                             "Confirm to restart, Back to exit");
    }

    const bool useFull = fullRefresh || firstRender_;
    renderer.displayBuffer(useFull ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    firstRender_ = false;
  }
};
