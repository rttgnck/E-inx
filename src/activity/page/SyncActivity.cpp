/**
 * @file SyncActivity.cpp
 * @brief Definitions for SyncActivity.
 */

#include "../page/SyncActivity.h"

#include <GfxRenderer.h>

#include "activity/network/BackupRestoreActivity.h"
#include "activity/reader/ImageViewerActivity.h"
#include "activity/settings/DictionaryPickerActivity.h"
#include "activity/settings/KOReaderSettingsActivity.h"
#include "activity/settings/OtaUpdateActivity.h"
#include "state/SystemSetting.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/MenuNav.h"
#include "system/UiTheme.h"

namespace {
// E-inx's network modes first (indices 0-6 are what onModeSelected receives), then upstream Inx's
// sub-activities. Keep MenuItem and MENU_ITEMS in the same order.
enum MenuItem {
  JOIN_NETWORK,
  FIRMWARE_UPDATE,
  LIBRARY_SERVER,
  BLUETOOTH_TRANSFER,
  CONNECT_CALIBRE,
  CREATE_HOTSPOT,
  OPDS_BROWSER,
  BACKUP_RESTORE,
  KOREADER_SYNC,
  CHECK_FOR_UPDATES,
  CHOOSE_DICTIONARY,
  DEVICE_IDENTITY,
  MENU_ITEM_COUNT
};
const char* MENU_ITEMS[MENU_ITEM_COUNT] = {"Join a Network",     "Firmware Update",    "Library Server",
                                           "Bluetooth Transfer", "Connect to Calibre", "Create Hotspot",
                                           "OPDS Browser",       "Backup and Restore", "KOReader Sync",
                                           "Check for Updates",  "Choose Dictionary",  "Device"};
constexpr int LIST_ITEM_HEIGHT = UiTheme::DRAWER_LIST_ITEM_HEIGHT;
}  // namespace

/**
 * Lifecycle hook called when entering the activity.
 */
void SyncActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;

  render();
  SETTINGS.runHalfRefreshOnLoadIfEnabled(renderer, SystemSetting::RefreshOnLoadPage::Sync);
}

/**
 * Main loop for handling user input and updating the display state.
 * Processes button presses for menu navigation and tab switching.
 */
void SyncActivity::loop() {
  if (subActivity) {
    ActivityWithSubactivity::loop();
    return;
  }

  if (tabSelectorIndex == 4 && updateRequired) {
    updateRequired = false;
    render();
  }

  const bool confirmPressed = mappedInput.wasPressed(MappedInputManager::Button::Confirm);
  const bool upPressed = mappedInput.wasPressed(MenuNav::itemPrev());
  const bool downPressed = mappedInput.wasPressed(MenuNav::itemNext());

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() >= 300 && onRecentOpen) {
      vTaskDelay(pdMS_TO_TICKS(300));
      onRecentOpen();
    }
    return;
  }

  if (mappedInput.wasPressed(MenuNav::tabPrev())) {
    tabSelectorIndex = 3;
    navigateToSelectedMenu();
    return;
  }

  if (mappedInput.wasPressed(MenuNav::tabNext())) {
    tabSelectorIndex = 5;
    navigateToSelectedMenu();
    return;
  }

  if (tabSelectorIndex != 4) {
    return;
  }

  if (confirmPressed) {
    const auto onSubActivityDone = [this] {
      exitActivity();
      updateRequired = true;
    };
    switch (selectedIndex) {
      case BACKUP_RESTORE:
        enterNewActivity(new BackupRestoreActivity(renderer, mappedInput, onSubActivityDone));
        return;
      case KOREADER_SYNC:
        enterNewActivity(new KOReaderSettingsActivity(renderer, mappedInput, onSubActivityDone));
        return;
      case CHECK_FOR_UPDATES:
        enterNewActivity(new OtaUpdateActivity(renderer, mappedInput, onSubActivityDone));
        return;
      case CHOOSE_DICTIONARY:
        enterNewActivity(new DictionaryPickerActivity(renderer, mappedInput, onSubActivityDone));
        return;
      case DEVICE_IDENTITY:
        enterNewActivity(
            new ImageViewerActivity(renderer, mappedInput, "/sleep/device-identity.jpg", onSubActivityDone));
        return;
      default:
        break;
    }

    static constexpr NetworkMode kModes[] = {NetworkMode::JOIN_NETWORK,    NetworkMode::UPDATE_SERVER,
                                             NetworkMode::LIBRARY_SERVER,  NetworkMode::BLUETOOTH_TRANSFER,
                                             NetworkMode::CONNECT_CALIBRE, NetworkMode::CREATE_HOTSPOT,
                                             NetworkMode::OPDS_BROWSER};
    if (onModeSelected && selectedIndex >= 0 && selectedIndex <= OPDS_BROWSER) {
      onModeSelected(kModes[selectedIndex]);
    }
    return;
  }

  bool needUpdate = false;

  if (upPressed) {
    selectedIndex = (selectedIndex + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
    needUpdate = true;
  }

  if (downPressed) {
    selectedIndex = (selectedIndex + 1) % MENU_ITEM_COUNT;
    needUpdate = true;
  }

  if (needUpdate) {
    updateRequired = true;
  }
}

/**
 * Renders the complete sync activity view including menu items and tab bar.
 */
void SyncActivity::render() const {
  renderer.clearScreen();
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int contentBottom = mainContentBottom(renderer);

  renderTabBar(renderer);

  const int headerY = mainContentTop();
  const int headerHeight = mainHeaderHeight();
  const int headerTextY = headerY + (headerHeight - renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_12_FONT_ID)) / 2;
  renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, 20, headerTextY, "Device Management", true,
                       EpdFontFamily::BOLD);

  const int dividerY = headerY + headerHeight;
  renderer.line.render(0, dividerY, screenWidth, dividerY);

  const int listStartY = dividerY;
  const int visibleAreaHeight = (INX_THEME.mainTabsAtBottom() ? contentBottom : screenHeight - 80) - listStartY;

  // More items than fit: scroll just far enough to keep the selection on screen.
  const int visibleCount = visibleAreaHeight / LIST_ITEM_HEIGHT;
  const int firstVisible = selectedIndex >= visibleCount ? selectedIndex - visibleCount + 1 : 0;

  for (int i = firstVisible; i < MENU_ITEM_COUNT; i++) {
    const int itemY = listStartY + (i - firstVisible) * LIST_ITEM_HEIGHT;

    if (itemY < listStartY + visibleAreaHeight && itemY + LIST_ITEM_HEIGHT > listStartY) {
      const bool isSelected = (i == selectedIndex);

      if (isSelected) {
        renderer.rectangle.fill(0, itemY, screenWidth, LIST_ITEM_HEIGHT, static_cast<int>(GfxRenderer::FillTone::Ink));
      }

      const int textX = 20;
      const int titleY = itemY + (LIST_ITEM_HEIGHT - renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID)) / 2;

      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, textX, titleY, MENU_ITEMS[i], !isSelected);
      const int caretW = renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_10_FONT_ID, "›");
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, screenWidth - caretW - 30, titleY, "›", !isSelected);

      if (i < MENU_ITEM_COUNT - 1) {
        renderer.line.render(0, itemY + LIST_ITEM_HEIGHT - 1, screenWidth, itemY + LIST_ITEM_HEIGHT - 1, true,
                             LineRender::Style::Dotted);
      }
    }
  }

  const auto labels = mappedInput.mapLabels("« Recent", "Select", "", "");
  renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

/**
 * Lifecycle hook called when exiting the activity.
 */
void SyncActivity::onExit() { ActivityWithSubactivity::onExit(); }
