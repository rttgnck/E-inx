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

#include <algorithm>
#include <functional>
#include <string>

#include "../activity/Activity.h"
#include "../activity/Menu.h"
#include "battleship/BattleshipActivity.h"
#include "chess/ChessActivity.h"
#include "connections/ConnectionsActivity.h"
#include "dungeon/DungeonActivity.h"
#include "hackernews/HackerNewsActivity.h"
#include "insider/InsiderActivity.h"
#include "jaipur/JaipurActivity.h"
#include "murdle/MurdleActivity.h"
#include "solitaire/SolitaireActivity.h"
#include "study/StudyActivity.h"
#include "xkcd/XkcdActivity.h"
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
  static constexpr int APP_COUNT = 11;
  // Row geometry is derived from the fonts rather than fixed, because it was
  // fixed and wrong: the box was ITEM_HEIGHT - ITEM_PADDING tall and the
  // description ended at exactly that line, so every descender was clipped by
  // the border. Deriving it means a font change cannot silently re-break it.
  static constexpr int ROW_PAD = 12;       // above the name, below the description
  static constexpr int ROW_TEXT_GAP = 4;   // between the two lines
  static constexpr int ROW_GAP = 10;       // between one row's box and the next
  // What the button-hint bar and the bottom margin need, so a row is never drawn
  // underneath them.
  static constexpr int BOTTOM_RESERVE = 60;
  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);

  struct AppInfo {
    const char* name;
    const char* description;
  };

  // Games first, then the apps — upstream splits these into two folders, which
  // needs a level of hierarchy this list does not have. Ordering them is the
  // same grouping without the extra press.
  static inline constexpr AppInfo kApps[APP_COUNT] = {
      {"Chess", "A full engine on the device"},
      {"Solitaire", "Klondike, in landscape"},
      {"Battleship", "Lay out a fleet, then hunt one"},
      {"D&Diagrams", "A nonogram whose clues are a dungeon"},
      {"Jaipur", "The two-player trading game, solo"},
      {"Murdle", "A logic grid built through the solver"},
      {"Connections", "The daily word grid, with an archive"},
      {"Insider", "A party game for a table and one device"},
      {"Study", "Anki decks with the FSRS scheduler"},
      {"Hacker News", "The front page, kept on the card"},
      {"xkcd", "The archive, packed for the card"},
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
      case 4:
        app = new JaipurActivity(renderer, mappedInput, backCb);
        break;
      case 5:
        app = new MurdleActivity(renderer, mappedInput, backCb);
        break;
      case 6:
        app = new ConnectionsActivity(renderer, mappedInput, backCb);
        break;
      case 7:
        app = new InsiderActivity(renderer, mappedInput, backCb);
        break;
      case 8:
        app = new StudyActivity(renderer, mappedInput, backCb);
        break;
      case 9:
        app = new HackerNewsActivity(renderer, mappedInput, backCb);
        break;
      case 10:
        app = new XkcdActivity(renderer, mappedInput, backCb);
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
    const int nameH = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_14_FONT_ID);
    const int descH = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID);
    const int boxH = ROW_PAD + nameH + ROW_TEXT_GAP + descH + ROW_PAD;
    const int rowStride = boxH + ROW_GAP;
    const int textMaxWidth = sw - 88;

    // Eleven rows do not fit a 792px panel, so the list scrolls. The window is
    // derived from the selection rather than tracked as its own state: one fact
    // (which app is current) instead of two that can disagree, and a wrap from
    // the last row to the first lands the window correctly with no special case.
    const int listBottom = renderer.getScreenHeight() - BOTTOM_RESERVE;
    const int visibleRows = std::max(1, (listBottom - listStartY) / rowStride);
    int firstVisible = 0;
    if (APP_COUNT > visibleRows) {
      firstVisible = selectedIndex - visibleRows / 2;
      firstVisible = std::max(0, std::min(firstVisible, APP_COUNT - visibleRows));
    }
    const int lastVisible = std::min(APP_COUNT, firstVisible + visibleRows);

    for (int i = firstVisible; i < lastVisible; ++i) {
      const int itemY = listStartY + (i - firstVisible) * rowStride;
      const bool selected = (i == selectedIndex);

      if (selected) {
        renderer.rectangle.fill(24, itemY, sw - 48, boxH, kInk);
      } else {
        renderer.rectangle.render(24, itemY, sw - 48, boxH);
      }

      const int textX = 44;
      const int nameY = itemY + ROW_PAD;
      const int descY = nameY + nameH + ROW_TEXT_GAP;
      renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, textX, nameY, kApps[i].name, !selected,
                           EpdFontFamily::BOLD);
      const std::string description =
          renderer.text.truncate(ATKINSON_HYPERLEGIBLE_10_FONT_ID, kApps[i].description, textMaxWidth);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, textX, descY, description.c_str(), !selected);
    }

    // Says there is more, and roughly where you are in it. Without this a list
    // that scrolls looks like a list that ends.
    if (APP_COUNT > visibleRows) {
      const std::string position = std::to_string(selectedIndex + 1) + "/" + std::to_string(APP_COUNT);
      const int width = renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_10_FONT_ID, position.c_str());
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, sw - 30 - width, contentY + 6, position.c_str());
    }

    renderButtonHints(renderer, "", "Open", "", "");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
};
