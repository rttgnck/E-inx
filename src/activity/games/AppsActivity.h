#pragma once

#include <HalDisplay.h>

#include <functional>

#include "../Activity.h"
#include "../Menu.h"
#include "system/Fonts.h"

class AppsActivity final : public Activity, public Menu {
 public:
  explicit AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        const std::function<void()>& onRecentOpen, const std::function<void()>& onLibraryOpen,
                        const std::function<void()>& onOpenNews, const std::function<void()>& onOpenGames)
      : Activity("Apps", renderer, mappedInput),
        Menu(),
        onRecentOpen(onRecentOpen),
        onLibraryOpen(onLibraryOpen),
        onOpenNews(onOpenNews),
        onOpenGames(onOpenGames) {
    tabSelectorIndex = 1;
  }

  void onEnter() override {
    Activity::onEnter();
    selectedIndex = 0;
    updateRequired = true;
  }

  void onExit() override {}

  void loop() override {
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
      switch (selectedIndex) {
        case 0:
          if (onOpenNews) onOpenNews();
          break;
        case 1:
          if (onOpenGames) onOpenGames();
          break;
      }
    }
  }

 private:
  static constexpr int APP_COUNT = 2;
  static constexpr int ITEM_HEIGHT = 80;
  static constexpr int ITEM_PADDING = 12;

  struct AppInfo {
    const char* name;
    const char* description;
  };

  static inline constexpr AppInfo kApps[APP_COUNT] = {
      {"News", "Daily news reader"},
      {"Games", "Tetris, Snake, Galaga, Maze Runner"},
  };

  const std::function<void()> onRecentOpen;
  const std::function<void()> onLibraryOpen;
  const std::function<void()> onOpenNews;
  const std::function<void()> onOpenGames;

  int selectedIndex = 0;
  bool updateRequired = false;

  void navigateToSelectedMenu() override {
    if (tabSelectorIndex == 0 && onRecentOpen) onRecentOpen();
    if (tabSelectorIndex == 2 && onLibraryOpen) onLibraryOpen();
  }

  void render() const {
    renderer.clearScreen();

    const int sw = renderer.getScreenWidth();

    if (!INX_THEME.mainTabsAtBottom()) {
      renderTabBar(renderer);
    }

    const int contentY = mainContentTop() + 12;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, 30, contentY, "Apps", true, EpdFontFamily::BOLD);
    renderer.line.render(30, contentY + 30, sw - 30, contentY + 30);

    const int listStartY = contentY + 46;
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
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, textX, descY, kApps[i].description, !selected);
    }

    renderButtonHints(renderer, "", "Open", "", "");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }

  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);
};
