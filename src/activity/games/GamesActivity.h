#pragma once

#include <HalDisplay.h>

#include <functional>
#include <string>

#include "../Activity.h"
#include "../Menu.h"
#include "GalagaActivity.h"
#include "MazeRunnerActivity.h"
#include "SnakeActivity.h"
#include "TetrisActivity.h"
#include "system/Fonts.h"

class GamesActivity final : public Activity, public Menu {
 public:
  explicit GamesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                         const std::function<void()>& onRecentOpen, const std::function<void()>& onLibraryOpen)
      : Activity("Games", renderer, mappedInput),
        Menu(),
        onRecentOpen(onRecentOpen),
        onLibraryOpen(onLibraryOpen) {
    tabSelectorIndex = 1;
  }

  ~GamesActivity() override {
    if (subGame) {
      delete subGame;
      subGame = nullptr;
    }
  }

  void onEnter() override {
    Activity::onEnter();
    selectedIndex = 0;
    updateRequired = true;
  }

  void onExit() override {
    if (subGame) {
      subGame->onExit();
      delete subGame;
      subGame = nullptr;
    }
  }

  void loop() override {
    if (subGame) {
      subGame->loop();
      return;
    }

    if (updateRequired) {
      updateRequired = false;
      render();
    }

    if (mappedInput.wasPressed(tabPrevButton())) {
      handleTabNavigation(true, false);
      return;
    }
    if (mappedInput.wasPressed(tabNextButton())) {
      handleTabNavigation(false, true);
      return;
    }
    if (tabSelectorIndex != 1) return;

    if (mappedInput.wasPressed(itemPrevButton())) {
      selectedIndex = (selectedIndex - 1 + GAME_COUNT) % GAME_COUNT;
      updateRequired = true;
    }
    if (mappedInput.wasPressed(itemNextButton())) {
      selectedIndex = (selectedIndex + 1) % GAME_COUNT;
      updateRequired = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      launchGame(selectedIndex);
    }
  }

  bool preventAutoSleep() override { return subGame != nullptr && subGame->preventAutoSleep(); }
  bool skipLoopDelay() override { return subGame != nullptr && subGame->skipLoopDelay(); }

 private:
  static constexpr int GAME_COUNT = 4;
  // Row geometry is derived from the fonts rather than fixed, because it was
  // fixed and wrong: the box was ITEM_HEIGHT - ITEM_PADDING tall and the
  // description ended at exactly that line, so every descender was clipped by
  // the border. Deriving it means a font change cannot silently re-break it.
  static constexpr int ROW_PAD = 12;       // above the name, below the description
  static constexpr int ROW_TEXT_GAP = 4;   // between the two lines
  static constexpr int ROW_GAP = 10;       // between one row's box and the next
  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);

  struct GameInfo {
    const char* name;
    const char* description;
  };

  static inline constexpr GameInfo kGames[GAME_COUNT] = {
      {"Tetris", "Classic block-stacking puzzle"},
      {"Snake", "Guide the snake to eat and grow"},
      {"Galaga", "Defend against alien waves"},
      {"Maze Runner", "Escape procedural 3D mazes"},
  };

  const std::function<void()> onRecentOpen;
  const std::function<void()> onLibraryOpen;

  int selectedIndex = 0;
  bool updateRequired = false;
  Activity* subGame = nullptr;

  void navigateToSelectedMenu() override {
    if (tabSelectorIndex == 0 && onRecentOpen) onRecentOpen();
    if (tabSelectorIndex == 2 && onLibraryOpen) onLibraryOpen();
  }

  void returnFromGame() {
    if (subGame) {
      subGame->onExit();
      delete subGame;
      subGame = nullptr;
    }
    updateRequired = true;
  }

  void launchGame(int index) {
    auto backCb = [this]() { returnFromGame(); };
    Activity* game = nullptr;
    switch (index) {
      case 0:
        game = new TetrisActivity(renderer, mappedInput, backCb);
        break;
      case 1:
        game = new SnakeActivity(renderer, mappedInput, backCb);
        break;
      case 2:
        game = new GalagaActivity(renderer, mappedInput, backCb);
        break;
      case 3:
        game = new MazeRunnerActivity(renderer, mappedInput, backCb);
        break;
      default:
        return;
    }
    subGame = game;
    subGame->onEnter();
  }

  void render() const {
    renderer.clearScreen();

    const int sw = renderer.getScreenWidth();

    renderTabBar(renderer);

    const int contentY = mainContentTop() + 8;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, 30, contentY, "Games", true, EpdFontFamily::BOLD);
    renderer.line.render(30, contentY + 30, sw - 30, contentY + 30);

    const int listStartY = contentY + 42;
    const int nameH = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_14_FONT_ID);
    const int descH = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID);
    const int boxH = ROW_PAD + nameH + ROW_TEXT_GAP + descH + ROW_PAD;
    const int rowStride = boxH + ROW_GAP;
    const int textMaxWidth = sw - 88;
    for (int i = 0; i < GAME_COUNT; ++i) {
      const int itemY = listStartY + i * rowStride;
      const bool selected = (i == selectedIndex);

      if (selected) {
        renderer.rectangle.fill(24, itemY, sw - 48, boxH, kInk);
      } else {
        renderer.rectangle.render(24, itemY, sw - 48, boxH);
      }

      const int textX = 44;
      const int nameY = itemY + ROW_PAD;
      const int descY = nameY + nameH + ROW_TEXT_GAP;
      renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, textX, nameY, kGames[i].name, !selected,
                           EpdFontFamily::BOLD);
      const std::string description =
          renderer.text.truncate(ATKINSON_HYPERLEGIBLE_10_FONT_ID, kGames[i].description, textMaxWidth);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, textX, descY, description.c_str(), !selected);
    }

    renderButtonHints(renderer, "", "Select", "", "");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
};
