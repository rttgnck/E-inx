/**
 * @file Menu.h
 * @brief Public interface and types for Menu.
 */

#ifndef BASE_TAB_ACTIVITY_H
#define BASE_TAB_ACTIVITY_H

#include <GfxRenderer.h>

#include "images/Library.h"
#include "images/News.h"
#include "images/Recent.h"
#include "images/Setting.h"
#include "images/Stats.h"
#include "images/Sync.h"
#include "state/SystemSetting.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/MenuNav.h"
#include "system/ScreenComponents.h"
#include "system/UiTheme.h"

class Menu {
 protected:
  static constexpr int TAB_BAR_HEIGHT = 65;
  static constexpr int TAB_COUNT = 6;
  static constexpr int ICON_SIZE = 40;
  static constexpr int BATTERY_Y = 30;
  static constexpr int SELECTED_BORDER_HEIGHT = 5;
  int tabSelectorIndex = 0;

  /**
   * @brief Default constructor
   */
  Menu() = default;
  /**
   * @brief Default destructor
   */
  virtual ~Menu() = default;

  // Main-menu navigation axis (see MenuNav). Front: Left/Right tabs, Up/Down items; side: swapped.
  /**
   * @brief Returns the button mapped to the previous tab
   */
  MappedInputManager::Button tabPrevButton() const { return MenuNav::tabPrev(); }
  /**
   * @brief Returns the button mapped to the next tab
   */
  MappedInputManager::Button tabNextButton() const { return MenuNav::tabNext(); }
  /**
   * @brief Returns the button mapped to the previous item
   */
  MappedInputManager::Button itemPrevButton() const { return MenuNav::itemPrev(); }
  /**
   * @brief Returns the button mapped to the next item
   */
  MappedInputManager::Button itemNextButton() const { return MenuNav::itemNext(); }

  /**
   * @brief Renders the tab bar with icons and selection indicator
   * @param renderer Reference to the graphics renderer (const)
   */
  void renderTabBar(const GfxRenderer& renderer) const {
    const int screenWidth = renderer.getScreenWidth();
    const int tabButtonWidth = (screenWidth / TAB_COUNT) - 1;

    for (int i = 0; i < TAB_COUNT; ++i) {
      int buttonX = i * tabButtonWidth;
      bool isSelected = (tabSelectorIndex == i);
      int iconX = buttonX + (tabButtonWidth - ICON_SIZE) / 2;
      int iconY = (TAB_BAR_HEIGHT - ICON_SIZE) / 2 + 5;

      switch (i) {
        case 0:
          renderer.bitmap.icon(Recent, iconX, iconY, ICON_SIZE, ICON_SIZE);
          break;
        case 1:
          renderer.bitmap.icon(News, iconX, iconY, ICON_SIZE, ICON_SIZE);
          break;
        case 2:
          renderer.bitmap.icon(Library, iconX, iconY, ICON_SIZE, ICON_SIZE);
          break;
        case 3:
          renderer.bitmap.icon(Setting, iconX, iconY, ICON_SIZE, ICON_SIZE);
          break;
        case 4:
          renderer.bitmap.icon(Sync, iconX, iconY, ICON_SIZE, ICON_SIZE);
          break;
        case 5:
          renderer.bitmap.icon(Stats, iconX, iconY, ICON_SIZE, ICON_SIZE);
          break;
      }

      if (isSelected) {
        renderer.rectangle.fill(iconX - 10, TAB_BAR_HEIGHT - 2, 60, SELECTED_BORDER_HEIGHT,
                                static_cast<int>(GfxRenderer::FillTone::Ink));
      }

      renderer.line.render(buttonX, TAB_BAR_HEIGHT, buttonX + tabButtonWidth, TAB_BAR_HEIGHT);
    }
    drawBattery(renderer);
  }

  void renderButtonHints(const GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                         const char* btn4) const {
    INX_THEME.drawButtonHints(renderer, ATKINSON_HYPERLEGIBLE_10_FONT_ID, btn1, btn2, btn3, btn4);
    if (INX_THEME.mainTabsAtBottom()) {
      renderTabBar(renderer);
    }
  }

  int mainContentTop() const { return INX_THEME.mainContentTop(); }
  int mainContentBottom(const GfxRenderer& renderer) const { return INX_THEME.mainContentBottom(renderer); }
  int mainHeaderDividerY() const { return mainContentTop() + TAB_BAR_HEIGHT; }

  /**
   * @brief Draws the battery icon and percentage on the screen
   * @param renderer Reference to the graphics renderer (const)
   */
  void drawBattery(const GfxRenderer& renderer) const {
    const int batteryX = renderer.getScreenWidth() - 80;
    if (SETTINGS.showBottomBarClock) {
      const std::string time = ScreenComponents::currentTimeText();
      if (!time.empty()) {
        const int width = renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_8_FONT_ID, time.c_str());
        renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, batteryX - width - 12, renderer.getScreenHeight() - 30,
                             time.c_str());
      }
    }
    ScreenComponents::drawBattery(
        renderer, batteryX, renderer.getScreenHeight() - 30,
        SETTINGS.hideBatteryPercentage != SystemSetting::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);
  }

  /**
   * @brief Pure virtual function to navigate to the selected menu tab
   * Must be implemented by derived classes
   */
  virtual void navigateToSelectedMenu() = 0;

  /**
   * @brief Handles left/right navigation between tabs
   * @param leftPressed True if left button is pressed
   * @param rightPressed True if right button is pressed
   */
  void handleTabNavigation(bool leftPressed, bool rightPressed) {
    if (leftPressed) {
      tabSelectorIndex = (tabSelectorIndex - 1 + TAB_COUNT) % TAB_COUNT;
      navigateToSelectedMenu();
    }
    if (rightPressed) {
      tabSelectorIndex = (tabSelectorIndex + 1) % TAB_COUNT;
      navigateToSelectedMenu();
    }
  }
};

#endif
