/**
 * @file OtaUpdateActivity.cpp
 * @brief Definitions for OtaUpdateActivity.
 */

#include "OtaUpdateActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <string>

#include "activity/network/WifiSelectionActivity.h"
#include "network/OtaUpdater.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/MenuNav.h"
#include "system/UiTheme.h"

// cppcheck-suppress missingInclude
#include "esp_task_wdt.h"

namespace {
constexpr int kSourceItemHeight = UiTheme::DRAWER_LIST_ITEM_HEIGHT;
constexpr int kFirmwareItemHeight = UiTheme::DRAWER_LIST_ITEM_HEIGHT;
constexpr int kReleaseNotesFont = ATKINSON_HYPERLEGIBLE_8_FONT_ID;
const std::string kEmptyPath;

bool hasBinExtension(const std::string& path) {
  if (path.length() < 4) {
    return false;
  }
  const size_t n = path.length();
  return (path[n - 4] == '.') && (path[n - 3] == 'b' || path[n - 3] == 'B') &&
         (path[n - 2] == 'i' || path[n - 2] == 'I') && (path[n - 1] == 'n' || path[n - 1] == 'N');
}

std::string joinSdPath(const char* dirPath, const char* name) {
  std::string n = name ? name : "";
  if (n.empty()) {
    return "";
  }
  if (n[0] == '/') {
    return n;
  }
  std::string d = dirPath ? dirPath : "/";
  if (d.empty() || d == "/") {
    return "/" + n;
  }
  if (d.back() == '/') {
    return d + n;
  }
  return d + "/" + n;
}

std::string fileNameFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return path;
  }
  return path.substr(slash + 1);
}

std::string formatBytes(const size_t bytes) {
  char buffer[24];
  if (bytes >= 1024 * 1024) {
    snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buffer, sizeof(buffer), "%u B", static_cast<unsigned>(bytes));
  }
  return std::string(buffer);
}

void drawUpdateProgressCard(const GfxRenderer& renderer, const int pageWidth, const int bodyTop, const int screenHeight,
                            const float progress, const size_t processedBytes, const size_t totalBytes,
                            const bool downloadingFromGithub) {
  const int centerY = bodyTop + (screenHeight - bodyTop - 80) / 2;

  renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY - 92, "INSTALLING UPDATE", true, EpdFontFamily::BOLD);
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, centerY - 54,
                         downloadingFromGithub ? "Downloading + installing" : "Installing firmware", true,
                         EpdFontFamily::BOLD);
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY - 10, "Please keep the device powered on.", true,
                         EpdFontFamily::REGULAR);

  const int barW = std::min(300, pageWidth - 72);
  constexpr int barH = 6;
  const int barX = (pageWidth - barW) / 2;
  const int barY = centerY + 28;
  renderer.rectangle.render(barX, barY, barW, barH, true);

  const int clamped = std::max(0, std::min(100, static_cast<int>(progress * 100.0f + 0.5f)));
  const int innerW = std::max(1, barW - 2);
  const int fillW = innerW * clamped / 100;
  renderer.rectangle.fill(barX + 1, barY + 1, innerW, barH - 2, false);
  if (fillW > 0) {
    renderer.rectangle.fill(barX + 1, barY + 1, fillW, barH - 2, true);
  }

  std::string metaLine;
  if (totalBytes > 0) {
    metaLine = std::to_string(clamped) + "% - " + formatBytes(processedBytes) + " / " + formatBytes(totalBytes);
  } else {
    metaLine = "Preparing package";
  }
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, barY + 26, metaLine.c_str(), true, EpdFontFamily::REGULAR);
}
}  // namespace

void OtaUpdateActivity::taskTrampoline(void* param) {
  auto* self = static_cast<OtaUpdateActivity*>(param);
  self->displayTaskLoop();
}

void OtaUpdateActivity::onWifiSelectionComplete(const bool success) {
  exitActivity();

  if (!success) {
    Serial.printf("[%lu] [OTA] WiFi connection failed, exiting\n", millis());
    goBack();
    return;
  }

  Serial.printf("[%lu] [OTA] WiFi connected, checking for update\n", millis());

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = CHECKING_FOR_UPDATE;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
  vTaskDelay(pdMS_TO_TICKS(450));
  Serial.printf("[%lu] [OTA] free heap before update check: %u bytes\n", millis(),
                static_cast<unsigned>(ESP.getFreeHeap()));

  const auto res = updater.checkForUpdate();
  if (res != OtaUpdater::OK) {
    Serial.printf("[%lu] [OTA] Update check failed: %d\n", millis(), res);
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = FAILED;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  if (!updater.isUpdateNewer()) {
    Serial.printf("[%lu] [OTA] No new update available\n", millis());
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    state = NO_UPDATE;
    xSemaphoreGive(renderingMutex);
    updateRequired = true;
    return;
  }

  prepareReleaseNotes();
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  state = WAITING_CONFIRMATION;
  xSemaphoreGive(renderingMutex);
  updateRequired = true;
}

void OtaUpdateActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = SOURCE_SELECTION;
  sourceSelectedIndex = 0;
  updateRequired = true;

  xTaskCreate(&OtaUpdateActivity::taskTrampoline, "OtaUpdateActivityTask", 4096, this, 1, &displayTaskHandle);

  Serial.printf("[%lu] [OTA] Waiting for update source selection\n", millis());
}

void OtaUpdateActivity::prepareReleaseNotes() {
  releaseNoteLines.clear();
  releaseNotesScrollOffset = 0;

  std::string text = updater.getReleaseNotes();
  if (text.empty()) {
    releaseNoteLines.push_back("No changelog was supplied with this release.");
    return;
  }
  if (text.size() > 8192) {
    text.resize(8192);
    text += "\n[Changelog truncated on device]";
  }

  // Simplify common Markdown markers for the reader's plain-text display.
  std::string plain;
  plain.reserve(text.size());
  bool atLineStart = true;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '\r') continue;
    if (atLineStart) {
      if (c == '#') continue;
      if ((c == '-' || c == '*') && i + 1 < text.size() && text[i + 1] == ' ') {
        plain += "•";
        atLineStart = false;
        continue;
      }
      if (c == ' ') continue;
    }
    plain += c;
    atLineStart = c == '\n';
  }

  const int maxWidth = renderer.getScreenWidth() - 36;
  std::string current;
  std::string word;
  const auto flushCurrent = [&]() {
    if (!current.empty()) {
      releaseNoteLines.push_back(current);
      current.clear();
    }
  };
  const auto addWord = [&]() {
    if (word.empty()) return;
    const std::string candidate = current.empty() ? word : current + " " + word;
    if (!current.empty() && renderer.text.getWidth(kReleaseNotesFont, candidate.c_str()) > maxWidth) {
      flushCurrent();
    }
    if (renderer.text.getWidth(kReleaseNotesFont, word.c_str()) > maxWidth) {
      releaseNoteLines.push_back(renderer.text.truncate(kReleaseNotesFont, word.c_str(), maxWidth));
    } else {
      current = current.empty() ? word : current + " " + word;
    }
    word.clear();
  };

  for (size_t i = 0; i <= plain.size(); ++i) {
    const char c = i < plain.size() ? plain[i] : '\n';
    if (c == '\n') {
      addWord();
      if (current.empty()) {
        if (releaseNoteLines.empty() || !releaseNoteLines.back().empty()) releaseNoteLines.push_back("");
      } else {
        flushCurrent();
      }
    } else if (c == ' ' || c == '\t') {
      addWord();
    } else {
      word.push_back(c);
    }
  }

  while (!releaseNoteLines.empty() && releaseNoteLines.back().empty()) releaseNoteLines.pop_back();
  if (releaseNoteLines.empty()) releaseNoteLines.push_back("No changelog was supplied with this release.");
}

void OtaUpdateActivity::scanSdFirmwareFiles() {
  sdFirmwareFiles.clear();
  sdFirmwareSelectedIndex = 0;
  sdFirmwareScrollOffset = 0;

  auto scanDir = [this](const char* dirPath) {
    FsFile dir = SdMan.open(dirPath);
    if (!dir || !dir.isDirectory()) {
      if (dir) {
        dir.close();
      }
      return;
    }

    for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (!file.isDirectory()) {
        char name[160] = {};
        file.getName(name, sizeof(name));
        const std::string path = joinSdPath(dirPath, name);
        if (hasBinExtension(path)) {
          sdFirmwareFiles.push_back(path);
        }
      }
      file.close();
    }
    dir.close();
  };

  scanDir("/");
  scanDir("/firmware");

  std::sort(sdFirmwareFiles.begin(), sdFirmwareFiles.end());
  sdFirmwareFiles.erase(std::unique(sdFirmwareFiles.begin(), sdFirmwareFiles.end()), sdFirmwareFiles.end());
}

const std::string& OtaUpdateActivity::selectedSdFirmwarePath() const {
  if (sdFirmwareSelectedIndex < 0 || sdFirmwareSelectedIndex >= static_cast<int>(sdFirmwareFiles.size())) {
    return kEmptyPath;
  }
  return sdFirmwareFiles[static_cast<size_t>(sdFirmwareSelectedIndex)];
}

void OtaUpdateActivity::onExit() {
  ActivityWithSubactivity::onExit();

  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void OtaUpdateActivity::displayTaskLoop() {
  while (true) {
    bool shouldRender = updateRequired;
    int progressPercent = -1;
    if (state == UPDATE_IN_PROGRESS) {
      const size_t processed = updater.getProcessedSize();
      const size_t total = updater.getTotalSize();
      progressPercent = total > 0 ? std::min(100, static_cast<int>((processed * 100ULL) / total)) : 0;
      shouldRender = shouldRender || displayedProgressPercent < 0 || progressPercent >= displayedProgressPercent + 5 ||
                     (progressPercent == 100 && displayedProgressPercent < 100);
    } else {
      displayedProgressPercent = -1;
    }
    if (shouldRender) {
      updateRequired = false;
      if (progressPercent >= 0) displayedProgressPercent = progressPercent;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void OtaUpdateActivity::render() {
  if (subActivity) {
    return;
  }

  float updaterProgress = 0;
  if (state == UPDATE_IN_PROGRESS && updater.getTotalSize() > 0) {
    updaterProgress = static_cast<float>(updater.getProcessedSize()) / static_cast<float>(updater.getTotalSize());
  }

  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  const int startY = 0;
  const int dividerY = INX_THEME.drawPageHeader(renderer, "Update", startY);

  const int bodyTop = dividerY;

  if (state == SOURCE_SELECTION) {
    constexpr const char* items[] = {"GitHub firmware update", "SD card firmware"};
    for (int i = 0; i < 2; ++i) {
      const int itemY = bodyTop + i * kSourceItemHeight;
      const bool selected = sourceSelectedIndex == i;
      if (selected) {
        renderer.rectangle.fill(0, itemY, pageWidth, kSourceItemHeight, static_cast<int>(GfxRenderer::FillTone::Ink));
      }
      const int textY = itemY + (kSourceItemHeight - renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID)) / 2;
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 20, textY, items[i], !selected, EpdFontFamily::REGULAR);
      renderer.line.render(0, itemY + kSourceItemHeight - 1, pageWidth, itemY + kSourceItemHeight - 1, true,
                           LineRender::Style::Dotted);
    }
    const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
    renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == WIFI_SELECTION) {
    const int centerY = dividerY + (screenHeight - dividerY - 80) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY, "Choose a network above.", true,
                           EpdFontFamily::REGULAR);
    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == CHECKING_FOR_UPDATE) {
    const int centerY = dividerY + (screenHeight - dividerY - 80) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY, "Checking GitHub releases...", true,
                           EpdFontFamily::REGULAR);
  } else if (state == WAITING_CONFIRMATION) {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 18, bodyTop + 2, "Current: " INX_VERSION, true,
                         EpdFontFamily::REGULAR);
    const std::string newVer =
        (reinstalling ? "Latest: " : "Available: ") + updater.getLatestVersion() + (reinstalling ? " (installed)" : "");
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 18, bodyTop + 29, newVer.c_str(), true,
                         EpdFontFamily::BOLD);
    renderer.line.render(18, bodyTop + 57, pageWidth - 18, bodyTop + 57, true, LineRender::Style::Dotted);

    const int firstLineY = bodyTop + 67;
    const int notesBottom = screenHeight - 44;
    const int lineHeight = renderer.text.getLineHeight(kReleaseNotesFont) + 4;
    const int visibleLines = std::max(1, (notesBottom - firstLineY) / lineHeight);
    const int maxScroll = std::max(0, static_cast<int>(releaseNoteLines.size()) - visibleLines);
    releaseNotesScrollOffset = std::max(0, std::min(releaseNotesScrollOffset, maxScroll));
    for (int i = 0; i < visibleLines && releaseNotesScrollOffset + i < static_cast<int>(releaseNoteLines.size());
         ++i) {
      const std::string& line = releaseNoteLines[static_cast<size_t>(releaseNotesScrollOffset + i)];
      if (!line.empty()) {
        renderer.text.render(kReleaseNotesFont, 18, firstLineY + i * lineHeight, line.c_str(), true,
                             EpdFontFamily::REGULAR);
      }
    }
    if (maxScroll > 0) {
      char position[20] = {};
      snprintf(position, sizeof(position), "%d/%d", releaseNotesScrollOffset + 1, maxScroll + 1);
      const int positionWidth = renderer.text.getWidth(kReleaseNotesFont, position);
      renderer.text.render(kReleaseNotesFont, pageWidth - positionWidth - 18, bodyTop + 59, position, true,
                           EpdFontFamily::REGULAR);
    }

    const auto labels =
        mappedInput.mapLabels(reinstalling ? "« Back" : "Cancel", reinstalling ? "Reinstall" : "Install", "Up", "Down");
    renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == WAITING_SD_SELECTION) {
    const int totalFiles = static_cast<int>(sdFirmwareFiles.size());
    if (totalFiles == 0) {
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 20, bodyTop, "No firmware .bin files found.", true,
                           EpdFontFamily::BOLD);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 20, bodyTop + 32, "Put .bin files in / or /firmware.",
                           true, EpdFontFamily::REGULAR);
      const auto labels = mappedInput.mapLabels("« Back", "", "", "");
      renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      const int listBottom = screenHeight - 44;
      const int visibleRows = std::max(1, (listBottom - bodyTop) / kFirmwareItemHeight);
      if (sdFirmwareSelectedIndex < sdFirmwareScrollOffset) {
        sdFirmwareScrollOffset = sdFirmwareSelectedIndex;
      } else if (sdFirmwareSelectedIndex >= sdFirmwareScrollOffset + visibleRows) {
        sdFirmwareScrollOffset = sdFirmwareSelectedIndex - visibleRows + 1;
      }
      const int maxScroll = std::max(0, totalFiles - visibleRows);
      sdFirmwareScrollOffset = std::max(0, std::min(sdFirmwareScrollOffset, maxScroll));

      const int endIndex = std::min(totalFiles, sdFirmwareScrollOffset + visibleRows);
      for (int i = sdFirmwareScrollOffset; i < endIndex; ++i) {
        const int itemY = bodyTop + (i - sdFirmwareScrollOffset) * kFirmwareItemHeight;
        const bool selected = sdFirmwareSelectedIndex == i;
        if (selected) {
          renderer.rectangle.fill(0, itemY, pageWidth, kFirmwareItemHeight,
                                  static_cast<int>(GfxRenderer::FillTone::Ink));
        }
        const std::string label = renderer.text.truncate(
            ATKINSON_HYPERLEGIBLE_10_FONT_ID, sdFirmwareFiles[static_cast<size_t>(i)].c_str(), pageWidth - 40);
        const int textY =
            itemY + (kFirmwareItemHeight - renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID)) / 2;
        renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 20, textY, label.c_str(), !selected,
                             EpdFontFamily::REGULAR);
        renderer.line.render(0, itemY + kFirmwareItemHeight - 1, pageWidth, itemY + kFirmwareItemHeight - 1, true,
                             LineRender::Style::Dotted);
      }
      const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
      renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state == WAITING_SD_CONFIRMATION) {
    const std::string& firmwarePath = selectedSdFirmwarePath();
    if (!firmwarePath.empty() && SdMan.exists(firmwarePath.c_str())) {
      FsFile file;
      size_t firmwareSize = 0;
      if (SdMan.openFileForRead("OTA", firmwarePath, file)) {
        firmwareSize = file.size();
        file.close();
      }
      const int centerY = dividerY + (screenHeight - dividerY - 80) / 2;
      const std::string fileName = renderer.text.truncate(ATKINSON_HYPERLEGIBLE_10_FONT_ID,
                                                          fileNameFromPath(firmwarePath).c_str(), pageWidth - 56);
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY - 92, "SD FIRMWARE", true, EpdFontFamily::BOLD);
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, centerY - 54, "Install update?", true,
                             EpdFontFamily::BOLD);
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY - 10, fileName.c_str(), true,
                             EpdFontFamily::REGULAR);
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 18, formatBytes(firmwareSize).c_str(), true,
                             EpdFontFamily::REGULAR);
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 58,
                             "Keep the device powered on during install.", true, EpdFontFamily::REGULAR);
      const auto labels = mappedInput.mapLabels("Cancel", "Install", "", "");
      renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    } else {
      const int centerY = dividerY + (screenHeight - dividerY - 80) / 2;
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY, "Firmware file is missing.", true,
                             EpdFontFamily::REGULAR);
      const auto labels = mappedInput.mapLabels("« Back", "", "", "");
      renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
  } else if (state == UPDATE_IN_PROGRESS) {
    drawUpdateProgressCard(renderer, pageWidth, bodyTop, screenHeight, updaterProgress, updater.getProcessedSize(),
                           updater.getTotalSize(), installingFromGithub);
  } else if (state == NO_UPDATE) {
    const int centerY = dividerY + (screenHeight - dividerY - 80) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY, "No update available", true, EpdFontFamily::BOLD);
    // A release was found, it just was not newer. Offering it anyway is the
    // whole point of this screen having a second button: assets do get replaced
    // under a tag, and a bad flash needs a way back without an SD card.
    const bool canReinstall = !updater.getLatestVersion().empty();
    if (canReinstall) {
      const std::string current = "Latest release: " + updater.getLatestVersion();
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 34, current.c_str(), true,
                             EpdFontFamily::REGULAR);
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 58,
                             "You are on it. See Latest to review or reinstall it.", true, EpdFontFamily::REGULAR);
    }
    const auto labels = mappedInput.mapLabels("« Back", canReinstall ? "See Latest" : "", "", "");
    renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FAILED) {
    const int centerY = dividerY + (screenHeight - dividerY - 80) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY, "Update failed", true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == FINISHED) {
    const int centerY = dividerY + (screenHeight - dividerY - 80) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY, "Update complete", true, EpdFontFamily::BOLD);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY + 50,
                           "Press and hold power button to turn back on", true, EpdFontFamily::REGULAR);
  } else if (state == SHUTTING_DOWN) {
    const int centerY = dividerY + (screenHeight - dividerY - 80) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY, "Update complete", true, EpdFontFamily::BOLD);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY + 50,
                           "Press and hold power button to turn back on", true, EpdFontFamily::REGULAR);
  }

  renderer.displayBuffer();

  if (state == FINISHED) {
    state = SHUTTING_DOWN;
  }
}

void OtaUpdateActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (state == SOURCE_SELECTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
      return;
    }
    if (mappedInput.wasPressed(MenuNav::itemPrev()) || mappedInput.wasPressed(MenuNav::itemNext())) {
      sourceSelectedIndex = sourceSelectedIndex == 0 ? 1 : 0;
      updateRequired = true;
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (sourceSelectedIndex == 0) {
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        state = WIFI_SELECTION;
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        Serial.printf("[%lu] [OTA] Turning on WiFi...\n", millis());
        WiFi.mode(WIFI_STA);
        Serial.printf("[%lu] [OTA] Launching WifiSelectionActivity...\n", millis());
        enterNewActivity(new WifiSelectionActivity(
            renderer, mappedInput, [this](const bool connected) { onWifiSelectionComplete(connected); }));
      } else {
        scanSdFirmwareFiles();
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        state = WAITING_SD_SELECTION;
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
      }
      return;
    }
    return;
  }

  if (state == WIFI_SELECTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == WAITING_CONFIRMATION) {
    const int lineHeight = renderer.text.getLineHeight(kReleaseNotesFont) + 4;
    const int visibleLines =
        std::max(1, (renderer.getScreenHeight() - 44 - (UiTheme::DRAWER_PAGE_HEADER_HEIGHT + 67)) / lineHeight);
    const int maxScroll = std::max(0, static_cast<int>(releaseNoteLines.size()) - visibleLines);
    if (mappedInput.wasPressed(MenuNav::itemPrev())) {
      releaseNotesScrollOffset = std::max(0, releaseNotesScrollOffset - 1);
      updateRequired = true;
      return;
    }
    if (mappedInput.wasPressed(MenuNav::itemNext())) {
      releaseNotesScrollOffset = std::min(maxScroll, releaseNotesScrollOffset + 1);
      updateRequired = true;
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      Serial.printf("[%lu] [OTA] %s, starting download...\n", millis(),
                    reinstalling ? "Reinstalling the current release" : "New update available");
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      installingFromGithub = true;
      state = UPDATE_IN_PROGRESS;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      vTaskDelay(10 / portTICK_PERIOD_MS);
      const auto res = updater.installUpdate(reinstalling);

      if (res != OtaUpdater::OK) {
        Serial.printf("[%lu] [OTA] Update failed: %d\n", millis(), res);
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        state = FAILED;
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        return;
      }

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = FINISHED;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (reinstalling) {
        // Back out to the screen this was reached from, not out of the activity.
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        reinstalling = false;
        state = NO_UPDATE;
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        return;
      }
      goBack();
    }

    return;
  }

  if (state == WAITING_SD_SELECTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = SOURCE_SELECTION;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      return;
    }

    const int totalFiles = static_cast<int>(sdFirmwareFiles.size());
    if (totalFiles == 0) {
      return;
    }

    if (mappedInput.wasPressed(MenuNav::itemNext())) {
      sdFirmwareSelectedIndex = (sdFirmwareSelectedIndex + 1) % totalFiles;
      updateRequired = true;
      return;
    }

    if (mappedInput.wasPressed(MenuNav::itemPrev())) {
      sdFirmwareSelectedIndex = (sdFirmwareSelectedIndex + totalFiles - 1) % totalFiles;
      updateRequired = true;
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = WAITING_SD_CONFIRMATION;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      return;
    }

    return;
  }

  if (state == WAITING_SD_CONFIRMATION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = WAITING_SD_SELECTION;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      return;
    }

    const std::string& firmwarePath = selectedSdFirmwarePath();
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !firmwarePath.empty() &&
        SdMan.exists(firmwarePath.c_str())) {
      Serial.printf("[%lu] [OTA] Installing firmware from SD: %s\n", millis(), firmwarePath.c_str());
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      installingFromGithub = false;
      state = UPDATE_IN_PROGRESS;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
      vTaskDelay(10 / portTICK_PERIOD_MS);
      const auto res = updater.installUpdateFromSd(firmwarePath.c_str());

      if (res != OtaUpdater::OK) {
        Serial.printf("[%lu] [OTA] SD update failed: %d\n", millis(), res);
        xSemaphoreTake(renderingMutex, portMAX_DELAY);
        state = FAILED;
        xSemaphoreGive(renderingMutex);
        updateRequired = true;
        return;
      }

      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      state = FINISHED;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
    }
    return;
  }

  if (state == FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
    }
    return;
  }

  if (state == NO_UPDATE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      goBack();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) && !updater.getLatestVersion().empty()) {
      prepareReleaseNotes();
      releaseNotesScrollOffset = 0;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      reinstalling = true;
      state = WAITING_CONFIRMATION;
      xSemaphoreGive(renderingMutex);
      updateRequired = true;
    }
    return;
  }

  if (state == SHUTTING_DOWN) {
    ESP.restart();
  }
}
