/**
 * @file ReadingStatsBackupActivity.cpp
 * @brief Definitions for ReadingStatsBackupActivity.
 */

#include "ReadingStatsBackupActivity.h"

#include <GfxRenderer.h>

#include <cstdio>

#include "state/Statistics.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"

void ReadingStatsBackupActivity::onEnter() {
  Activity::onEnter();
  state = State::CHOOSE;
  resultMessage.clear();
  render();
}

void ReadingStatsBackupActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == State::DONE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      onBack();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selected = selected == 0 ? 1 : 0;
    render();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    runSelected();
  }
}

void ReadingStatsBackupActivity::runSelected() {
  // Show progress: the copy is fast, but a full refresh is cheap and gives feedback on slow SD cards.
  resultMessage = selected == 0 ? "Backing up..." : "Restoring...";
  render();

  char buf[64];
  if (selected == 0) {
    const int n = backupAllBookStats(kBackupRoot);
    snprintf(buf, sizeof(buf), "Backed up %d book%s", n, n == 1 ? "" : "s");
  } else {
    const int n = restoreAllBookStats(kBackupRoot);
    snprintf(buf, sizeof(buf), "Restored %d book%s", n, n == 1 ? "" : "s");
  }
  resultMessage = buf;
  state = State::DONE;
  render();
}

void ReadingStatsBackupActivity::render() {
  renderer.clearScreen();
  const int h = renderer.getScreenHeight();

  if (state == State::DONE) {
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_12_FONT_ID, h / 2 - 20, "Reading Stats", true, EpdFontFamily::BOLD);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, h / 2 + 12, resultMessage.c_str(), true);
    const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "Done", "", "");
    renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  renderer.text.centered(ATKINSON_HYPERLEGIBLE_12_FONT_ID, h / 2 - 70, "Reading Stats Backup", true,
                         EpdFontFamily::BOLD);

  const char* options[] = {"Backup to SD card", "Restore from SD card"};
  const int rowH = 44;
  const int firstY = h / 2 - 24;
  const int pageWidth = renderer.getScreenWidth();
  for (int i = 0; i < 2; i++) {
    const int rowY = firstY + i * rowH;
    const bool sel = (i == selected);
    if (sel) {
      renderer.rectangle.fill(30, rowY - 6, pageWidth - 60, rowH - 8, true);
    }
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, rowY, options[i], !sel);
  }

  renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, firstY + 2 * rowH + 12,
                         "Backup writes /.backups/reading_stats/reading_stats.json", true);
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, firstY + 2 * rowH + 30,
                         "Restore reads it back (replaces current stats)", true);

  const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "Run", "", "");
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
