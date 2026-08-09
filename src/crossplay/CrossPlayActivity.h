#pragma once

/**
 * @file CrossPlayActivity.h
 * @brief The CrossPlay shelf: the app drawer's door into the ported apps.
 *
 * Upstream has src/apps_local/Shelf.cpp for this, built around CrossPoint's
 * `replaceActivity` and its activity stack — an app calls `leave()` and the
 * manager pops it back to the folder it came from. E-inx has one live activity
 * and no stack, so that file is deliberately not ported. This follows E-inx's
 * own GamesActivity instead: the menu owns the running app, hands it a callback,
 * and deletes it when the callback fires.
 *
 * Four entries today. The list is a table so that adding the next port is a row.
 */

#include <HalDisplay.h>

#include <functional>
#include <string>

#include "../activity/Activity.h"
#include "../activity/Menu.h"
#include "battleship/BattleshipActivity.h"
#include "chess/ChessActivity.h"
#include "dungeon/DungeonActivity.h"
#include "solitaire/SolitaireActivity.h"
#include "system/Fonts.h"

class CrossPlayActivityMenu final : public Activity, public Menu {
 public:
  explicit CrossPlayActivityMenu(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void()>& onRecentOpen,
                                 const std::function<void()>& onLibraryOpen)
      : Activity("CrossPlay", renderer, mappedInput),
        Menu(),
        onRecentOpen(onRecentOpen),
        onLibraryOpen(onLibraryOpen) {
    tabSelectorIndex = 1;
  }

  ~CrossPlayActivityMenu() override {
    delete subApp;
    subApp = nullptr;
  }

  void onEnter() override {
    Activity::onEnter();
    selectedIndex = 0;
    updateRequired = true;
  }

  void onExit() override {
    if (subApp) {
      subApp->onExit();
      delete subApp;
      subApp = nullptr;
    }
  }

  void loop() override {
    if (subApp) {
      subApp->loop();
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
      selectedIndex = (selectedIndex - 1 + APP_COUNT) % APP_COUNT;
      updateRequired = true;
    }
    if (mappedInput.wasPressed(itemNextButton())) {
      selectedIndex = (selectedIndex + 1) % APP_COUNT;
      updateRequired = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      launchApp(selectedIndex);
    }
  }

  bool preventAutoSleep() override { return subApp != nullptr && subApp->preventAutoSleep(); }
  bool skipLoopDelay() override { return subApp != nullptr && subApp->skipLoopDelay(); }

 private:
  static constexpr int APP_COUNT = 4;
  static constexpr int ITEM_HEIGHT = 64;
  static constexpr int ITEM_PADDING = 10;
  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);

  struct AppInfo {
    const char* name;
    const char* description;
  };

  static inline constexpr AppInfo kApps[APP_COUNT] = {
      {"Chess", "A full engine on the device"},
      {"Solitaire", "Klondike, in landscape"},
      {"Battleship", "Lay out a fleet, then hunt one"},
      {"D&Diagrams", "A nonogram whose clues are a dungeon"},
  };

  const std::function<void()> onRecentOpen;
  const std::function<void()> onLibraryOpen;

  int selectedIndex = 0;
  bool updateRequired = false;
  Activity* subApp = nullptr;

  void navigateToSelectedMenu() override {
    if (tabSelectorIndex == 0 && onRecentOpen) onRecentOpen();
    if (tabSelectorIndex == 2 && onLibraryOpen) onLibraryOpen();
  }

  void returnFromApp() {
    if (subApp) {
      subApp->onExit();
      delete subApp;
      subApp = nullptr;
    }
    updateRequired = true;
  }

  void launchApp(const int index) {
    auto backCb = [this]() { returnFromApp(); };
    Activity* app = nullptr;
    switch (index) {
      case 0:
        app = new ChessActivity(renderer, mappedInput, backCb);
        break;
      case 1:
        app = new SolitaireActivity(renderer, mappedInput, backCb);
        break;
      case 2:
        app = new BattleshipActivity(renderer, mappedInput, backCb);
        break;
      case 3:
        app = new DungeonActivity(renderer, mappedInput, backCb);
        break;
      default:
        break;
    }
    if (!app) return;
    subApp = app;
    subApp->onEnter();
  }

  void render() const {
    renderer.clearScreen();

    const int sw = renderer.getScreenWidth();

    renderTabBar(renderer);

    const int contentY = mainContentTop() + 8;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, 30, contentY, "CrossPlay", true, EpdFontFamily::BOLD);
    renderer.line.render(30, contentY + 30, sw - 30, contentY + 30);

    const int listStartY = contentY + 42;
    const int textMaxWidth = sw - 88;
    for (int i = 0; i < APP_COUNT; ++i) {
      const int itemY = listStartY + i * ITEM_HEIGHT;
      const bool selected = (i == selectedIndex);

      if (selected) {
        renderer.rectangle.fill(24, itemY, sw - 48, ITEM_HEIGHT - ITEM_PADDING, kInk);
      } else {
        renderer.rectangle.render(24, itemY, sw - 48, ITEM_HEIGHT - ITEM_PADDING);
      }

      const int textX = 44;
      const int nameY = itemY + 14;
      const int descY = itemY + 40;
      renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, textX, nameY, kApps[i].name, !selected,
                           EpdFontFamily::BOLD);
      const std::string description =
          renderer.text.truncate(ATKINSON_HYPERLEGIBLE_10_FONT_ID, kApps[i].description, textMaxWidth);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, textX, descY, description.c_str(), !selected);
    }

    renderButtonHints(renderer, "", "Open", "", "");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
};
