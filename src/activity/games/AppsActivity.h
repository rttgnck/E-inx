#pragma once

#include <HalDisplay.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "../Menu.h"
#include "state/SystemSetting.h"
#include "system/Fonts.h"

class AppsActivity final : public Activity, public Menu {
 public:
  explicit AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        const std::function<void()>& onRecentOpen, const std::function<void()>& onLibraryOpen,
                        const std::function<void()>& onOpenNews, const std::function<void()>& onOpenGames,
                        const std::function<void()>& onOpenCrossPlay,
                        const std::function<void()>& onOpenAgentIsland)
      : Activity("Apps", renderer, mappedInput),
        Menu(),
        onRecentOpen(onRecentOpen),
        onLibraryOpen(onLibraryOpen),
        onOpenNews(onOpenNews),
        onOpenGames(onOpenGames),
        onOpenCrossPlay(onOpenCrossPlay),
        onOpenAgentIsland(onOpenAgentIsland) {
    tabSelectorIndex = 1;
  }

  void onEnter() override {
    Activity::onEnter();
    buildVisibleApps();
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

    const int count = static_cast<int>(visibleApps.size());
    if (count == 0) return;

    if (mappedInput.wasPressed(itemPrevButton())) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      updateRequired = true;
    }
    if (mappedInput.wasPressed(itemNextButton())) {
      selectedIndex = (selectedIndex + 1) % count;
      updateRequired = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      switch (visibleApps[static_cast<size_t>(selectedIndex)]) {
        case App::News:
          if (onOpenNews) onOpenNews();
          break;
        case App::Games:
          if (onOpenGames) onOpenGames();
          break;
        case App::CrossPlay:
          if (onOpenCrossPlay) onOpenCrossPlay();
          break;
        case App::AgentIsland:
          if (onOpenAgentIsland) onOpenAgentIsland();
          break;
      }
    }
  }

 private:
  static constexpr int APP_COUNT = 4;
  // Row geometry is derived from the fonts rather than fixed, because it was
  // fixed and wrong: the box was ITEM_HEIGHT - ITEM_PADDING tall and the
  // description ended at exactly that line, so every descender was clipped by
  // the border. Deriving it means a font change cannot silently re-break it.
  static constexpr int ROW_PAD = 12;       // above the name, below the description
  static constexpr int ROW_TEXT_GAP = 4;   // between the two lines
  static constexpr int ROW_GAP = 10;       // between one row's box and the next

  enum class App : uint8_t { News, Games, CrossPlay, AgentIsland };

  struct AppInfo {
    const char* name;
    const char* description;
  };

  static inline constexpr AppInfo kApps[APP_COUNT] = {
      {"News", "Daily news reader"},
      {"Games", "Tetris, Snake, Galaga, Maze Runner"},
      {"CrossPlay", "Games and apps ported from CrossPlay"},
      {"AgentIsland", "Approvals and questions from your Mac"},
  };

  /** The drawer is whatever Settings › Display › App Drawer has left switched on. */
  void buildVisibleApps() {
    visibleApps.clear();
    if (SETTINGS.appDrawerNews) visibleApps.push_back(App::News);
    if (SETTINGS.appDrawerGames) visibleApps.push_back(App::Games);
    if (SETTINGS.appDrawerCrossPlay) visibleApps.push_back(App::CrossPlay);
    if (SETTINGS.appDrawerAgentIsland) visibleApps.push_back(App::AgentIsland);
  }

  std::vector<App> visibleApps;

  const std::function<void()> onRecentOpen;
  const std::function<void()> onLibraryOpen;
  const std::function<void()> onOpenNews;
  const std::function<void()> onOpenGames;
  const std::function<void()> onOpenCrossPlay;
  const std::function<void()> onOpenAgentIsland;

  int selectedIndex = 0;
  bool updateRequired = false;

  void navigateToSelectedMenu() override {
    if (tabSelectorIndex == 0 && onRecentOpen) onRecentOpen();
    if (tabSelectorIndex == 2 && onLibraryOpen) onLibraryOpen();
  }

  void render() const {
    renderer.clearScreen();

    const int sw = renderer.getScreenWidth();

    renderTabBar(renderer);

    const int contentY = mainContentTop() + 8;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, 30, contentY, "Apps", true, EpdFontFamily::BOLD);
    renderer.line.render(30, contentY + 30, sw - 30, contentY + 30);

    const int listStartY = contentY + 42;
    const int nameH = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_14_FONT_ID);
    const int descH = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID);
    const int boxH = ROW_PAD + nameH + ROW_TEXT_GAP + descH + ROW_PAD;
    const int rowStride = boxH + ROW_GAP;
    const int textMaxWidth = sw - 88;

    if (visibleApps.empty()) {
      renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, 30, listStartY + 10, "No apps are switched on.");
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 30, listStartY + 40,
                           "Settings › Display › App Drawer chooses what appears here.");
      renderButtonHints(renderer, "", "", "", "");
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }

    for (int i = 0; i < static_cast<int>(visibleApps.size()); ++i) {
      const AppInfo& app = kApps[static_cast<size_t>(visibleApps[static_cast<size_t>(i)])];
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
      renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, textX, nameY, app.name, !selected, EpdFontFamily::BOLD);
      const std::string description =
          renderer.text.truncate(ATKINSON_HYPERLEGIBLE_10_FONT_ID, app.description, textMaxWidth);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, textX, descY, description.c_str(), !selected);
    }

    renderButtonHints(renderer, "", "Open", "", "");
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }

  static constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);
};
