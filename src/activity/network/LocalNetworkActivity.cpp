/**
 * @file LocalNetworkActivity.cpp
 * @brief Definitions for LocalNetworkActivity.
 */

#include "LocalNetworkActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <algorithm>

#include "WifiSelectionActivity.h"
#include "state/NetworkCredential.h"
#include "state/SystemSetting.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/MenuNav.h"
#include "system/ScreenComponents.h"
#include "system/UiTheme.h"

namespace {

constexpr int CONTENT_MARGIN = 25;
constexpr int LINE_SPACING = 28;
constexpr int SMALL_SPACING = 25;
constexpr int SECTION_SPACING = 40;
constexpr int BOTTOM_AREA_HEIGHT = 80;
constexpr unsigned long SAVED_WIFI_TIMEOUT_MS = 15000;
constexpr int GITHUB_NOTES_FONT = ATKINSON_HYPERLEGIBLE_8_FONT_ID;
constexpr int GITHUB_NOTES_FIRST_OFFSET = 67;

/**
 * @brief Renders the header section for the activity
 * @param renderer Graphics renderer instance
 * @param startY Starting Y coordinate
 * @param title Header title text
 * @param subtitle Optional subtitle text
 */
int renderActivityHeader(const GfxRenderer& renderer, int startY, const char* title) {
  return INX_THEME.drawPageHeader(renderer, title, startY, nullptr, CONTENT_MARGIN);
}

/**
 * @brief Truncates a string to a maximum length, adding ellipsis if needed
 * @param str Input string to truncate
 * @param maxLength Maximum allowed length
 * @return Truncated string with ellipsis if original exceeds maxLength
 */
std::string truncateString(const std::string& str, int maxLength) {
  if (str.length() <= maxLength) return str;
  std::string result = str;
  result.replace(maxLength - 3, result.length() - (maxLength - 3), "...");
  return result;
}

std::string formatBytes(const size_t bytes) {
  char buffer[24] = {};
  if (bytes >= 1024 * 1024) {
    snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    snprintf(buffer, sizeof(buffer), "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    snprintf(buffer, sizeof(buffer), "%u B", static_cast<unsigned>(bytes));
  }
  return std::string(buffer);
}

const char* githubUpdateErrorMessage(const OtaUpdater::OtaUpdaterError error) {
  switch (error) {
    case OtaUpdater::NO_UPDATE:
      return "Release has no compatible firmware image";
    case OtaUpdater::HTTP_ERROR:
      return "Could not reach GitHub or download firmware";
    case OtaUpdater::JSON_PARSE_ERROR:
      return "Could not read the GitHub release details";
    case OtaUpdater::UPDATE_OLDER_ERROR:
      return "The published release is not newer";
    case OtaUpdater::OOM_ERROR:
      return "Not enough memory to complete the update";
    case OtaUpdater::INTERNAL_UPDATE_ERROR:
    default:
      return "The firmware update could not be completed";
  }
}
}  // namespace

/**
 * @brief Static trampoline function for FreeRTOS task creation
 * @param param Pointer to LocalNetworkActivity instance
 */
void LocalNetworkActivity::taskTrampoline(void* param) {
  auto* self = static_cast<LocalNetworkActivity*>(param);
  self->displayTaskLoop();
}

/**
 * @brief Initializes the activity and connects to WiFi
 */
void LocalNetworkActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  Serial.printf("[%lu] [LOCALNET] Starting local network mode\n", millis());
  Serial.printf("[%lu] [LOCALNET] [MEM] Free heap: %d bytes\n", millis(), ESP.getFreeHeap());

  renderingMutex = xSemaphoreCreateMutex();
  updateRequired = true;
  state = autoConnectSaved ? LocalNetworkState::WIFI_AUTO_CONNECTING : LocalNetworkState::WIFI_SELECTION;
  wifiSelectionCompletionPending = false;
  wifiSelectionConnected = false;

  xTaskCreate(&LocalNetworkActivity::taskTrampoline, "LocalNetTask", 4096, this, 1, &displayTaskHandle);

  if (autoConnectSaved) {
    startSavedWifiConnection();
  } else {
    startWifiSelection();
  }
}

void LocalNetworkActivity::startSavedWifiConnection() {
  WIFI_STORE.loadFromFile();
  const WifiCredential* credential = WIFI_STORE.getLastCredential();
  if (credential == nullptr || credential->ssid.empty()) {
    if (libraryLanding && onSwitchMode) {
      Serial.printf("[%lu] [LOCALNET] No saved WiFi network; starting library hotspot\n", millis());
      switchModePending = true;
      updateRequired = true;
      return;
    }
    Serial.printf("[%lu] [LOCALNET] No saved WiFi network; opening picker\n", millis());
    startWifiSelection();
    return;
  }

  connectedSSID = credential->ssid;
  connectedIP.clear();
  state = LocalNetworkState::WIFI_AUTO_CONNECTING;
  wifiConnectionStartTime = millis();
  updateRequired = true;

  WiFi.mode(WIFI_STA);
  // Before begin(), or DHCP never sees it: this is the name the router lists the reader under,
  // and what WiFi.getHostname() reports back to the web dashboard.
  WiFi.setHostname(SETTINGS.getDeviceHostname().c_str());
  WiFi.disconnect();
  delay(100);
  if (credential->password.empty()) {
    WiFi.begin(credential->ssid.c_str());
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }
  Serial.printf("[%lu] [LOCALNET] Connecting to saved WiFi: %s\n", millis(), credential->ssid.c_str());
}

void LocalNetworkActivity::startWifiSelection() {
  state = LocalNetworkState::WIFI_SELECTION;
  updateRequired = true;
  WiFi.mode(WIFI_STA);
  // Before begin(), or DHCP never sees it: this is the name the router lists the reader under,
  // and what WiFi.getHostname() reports back to the web dashboard.
  WiFi.setHostname(SETTINGS.getDeviceHostname().c_str());
  enterNewActivity(
      new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) { onWifiSelectionComplete(connected); }));
}

/**
 * @brief Cleans up resources when exiting the activity
 */
void LocalNetworkActivity::onExit() {
  ActivityWithSubactivity::onExit();

  stopWebServer();
  MDNS.end();

  if (renderingMutex) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    if (displayTaskHandle) {
      vTaskDelete(displayTaskHandle);
      displayTaskHandle = nullptr;
    }
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }
}

/**
 * @brief Callback handler for WiFi selection completion
 * @param connected True if WiFi connection successful
 */
void LocalNetworkActivity::onWifiSelectionComplete(const bool connected) {
  wifiSelectionConnected = connected;
  wifiSelectionCompletionPending = true;
  if (connected && subActivity) {
    connectedIP = static_cast<WifiSelectionActivity*>(subActivity.get())->getConnectedIP();
    connectedSSID = WiFi.SSID().c_str();
  }
}

void LocalNetworkActivity::finishWifiSelection() {
  const bool connected = wifiSelectionConnected;
  wifiSelectionCompletionPending = false;

  if (!connected) {
    Serial.printf("[%lu] [LOCALNET] WiFi selection cancelled\n", millis());
    if (onGoBack) onGoBack();
    return;
  }

  exitActivity();
  finishConnectedNetwork();
}

void LocalNetworkActivity::finishConnectedNetwork() {
  Serial.printf("[%lu] [LOCALNET] Connected to %s, IP: %s\n", millis(), connectedSSID.c_str(), connectedIP.c_str());

  state = LocalNetworkState::SERVER_STARTING;
  updateRequired = true;

  if (MDNS.begin(SETTINGS.getDeviceHostname().c_str())) {
    Serial.printf("[%lu] [LOCALNET] mDNS started: http://%s.local/\n", millis(),
                  SETTINGS.getDeviceHostname().c_str());
  }

  startWebServer();
}

/**
 * @brief Initializes and starts the web server for file transfers
 */
void LocalNetworkActivity::startWebServer() {
  Serial.printf("[%lu] [LOCALNET] Starting web server...\n", millis());

  webServer.reset(new LocalServer());
  webServer->begin();

  if (webServer->isRunning()) {
    state = LocalNetworkState::SERVER_RUNNING;
    Serial.printf("[%lu] [LOCALNET] Web server started successfully at http://%s/\n", millis(), connectedIP.c_str());

    updateRequired = true;
  } else {
    Serial.printf("[%lu] [LOCALNET] ERROR: Failed to start web server!\n", millis());
    webServer.reset();
    state = LocalNetworkState::ERROR;
    updateRequired = true;
  }
}

/**
 * @brief Stops the web server and cleans up resources
 */
void LocalNetworkActivity::stopWebServer() {
  if (webServer && webServer->isRunning()) {
    Serial.printf("[%lu] [LOCALNET] Stopping web server...\n", millis());
    webServer->stop();
  }
  webServer.reset();
}

void LocalNetworkActivity::prepareGithubReleaseNotes() {
  githubReleaseNoteLines.clear();
  githubReleaseNotesScrollOffset = 0;

  std::string text = githubUpdater.getReleaseNotes();
  if (text.empty()) {
    githubReleaseNoteLines.push_back("No changelog was supplied with this release.");
    return;
  }
  if (text.size() > 8192) {
    text.resize(8192);
    text += "\n[Changelog truncated on device]";
  }

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
      githubReleaseNoteLines.push_back(current);
      current.clear();
    }
  };
  const auto addWord = [&]() {
    if (word.empty()) return;
    const std::string candidate = current.empty() ? word : current + " " + word;
    if (!current.empty() && renderer.text.getWidth(GITHUB_NOTES_FONT, candidate.c_str()) > maxWidth) {
      flushCurrent();
    }
    if (renderer.text.getWidth(GITHUB_NOTES_FONT, word.c_str()) > maxWidth) {
      githubReleaseNoteLines.push_back(renderer.text.truncate(GITHUB_NOTES_FONT, word.c_str(), maxWidth));
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
        if (githubReleaseNoteLines.empty() || !githubReleaseNoteLines.back().empty()) {
          githubReleaseNoteLines.push_back("");
        }
      } else {
        flushCurrent();
      }
    } else if (c == ' ' || c == '\t') {
      addWord();
    } else {
      word.push_back(c);
    }
  }

  while (!githubReleaseNoteLines.empty() && githubReleaseNoteLines.back().empty()) {
    githubReleaseNoteLines.pop_back();
  }
  if (githubReleaseNoteLines.empty()) {
    githubReleaseNoteLines.push_back("No changelog was supplied with this release.");
  }
}

void LocalNetworkActivity::startGithubUpdateCheck() {
  if (!updateLanding || WiFi.status() != WL_CONNECTED) return;

  stopWebServer();
  state = LocalNetworkState::GITHUB_CHECKING;
  githubUpdateError.clear();
  githubReinstalling = false;
  updateRequired = true;
  vTaskDelay(pdMS_TO_TICKS(350));

  Serial.printf("[%lu] [LOCALNET] Checking GitHub firmware from Update Server screen\n", millis());
  const auto result = githubUpdater.checkForUpdate();
  if (result != OtaUpdater::OK) {
    githubUpdateError = githubUpdateErrorMessage(result);
    state = LocalNetworkState::GITHUB_FAILED;
  } else if (!githubUpdater.isUpdateNewer()) {
    state = LocalNetworkState::GITHUB_NO_UPDATE;
  } else {
    prepareGithubReleaseNotes();
    state = LocalNetworkState::GITHUB_CONFIRMATION;
  }
  updateRequired = true;
}

void LocalNetworkActivity::installGithubUpdate() {
  state = LocalNetworkState::GITHUB_INSTALLING;
  updateRequired = true;
  vTaskDelay(pdMS_TO_TICKS(50));

  Serial.printf("[%lu] [LOCALNET] Installing GitHub firmware %s\n", millis(),
                githubUpdater.getLatestVersion().c_str());
  const auto result = githubUpdater.installUpdate(githubReinstalling);
  if (result != OtaUpdater::OK) {
    githubUpdateError = githubUpdateErrorMessage(result);
    state = LocalNetworkState::GITHUB_FAILED;
    updateRequired = true;
    return;
  }

  state = LocalNetworkState::GITHUB_FINISHED;
  githubRestartAt = millis() + 2500;
  updateRequired = true;
}

void LocalNetworkActivity::returnToUpdateServer() {
  githubRestartAt = 0;
  if (WiFi.status() != WL_CONNECTED) {
    startSavedWifiConnection();
    return;
  }
  state = LocalNetworkState::SERVER_STARTING;
  updateRequired = true;
  startWebServer();
}

/**
 * @brief Main loop processing WiFi monitoring and web server requests
 */
void LocalNetworkActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    if (wifiSelectionCompletionPending) {
      finishWifiSelection();
    }
    return;
  }

  if (switchModePending) {
    switchModePending = false;
    if (onSwitchMode) onSwitchMode();
    return;
  }

  if (state == LocalNetworkState::GITHUB_FINISHED) {
    if (githubRestartAt != 0 && static_cast<long>(millis() - githubRestartAt) >= 0) {
      githubRestartAt = 0;
#ifndef SIMULATOR
      ESP.restart();
#endif
    }
    return;
  }

  if (state == LocalNetworkState::GITHUB_CONFIRMATION) {
    const int lineHeight = renderer.text.getLineHeight(GITHUB_NOTES_FONT) + 4;
    const int visibleLines =
        std::max(1, (renderer.getScreenHeight() - 44 -
                     (UiTheme::DRAWER_PAGE_HEADER_HEIGHT + GITHUB_NOTES_FIRST_OFFSET)) /
                        lineHeight);
    const int maxScroll = std::max(0, static_cast<int>(githubReleaseNoteLines.size()) - visibleLines);
    if (mappedInput.wasPressed(MenuNav::itemPrev())) {
      githubReleaseNotesScrollOffset = std::max(0, githubReleaseNotesScrollOffset - 1);
      updateRequired = true;
      return;
    }
    if (mappedInput.wasPressed(MenuNav::itemNext())) {
      githubReleaseNotesScrollOffset = std::min(maxScroll, githubReleaseNotesScrollOffset + 1);
      updateRequired = true;
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      installGithubUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (githubReinstalling) {
        githubReinstalling = false;
        state = LocalNetworkState::GITHUB_NO_UPDATE;
        updateRequired = true;
        return;
      }
      returnToUpdateServer();
    }
    return;
  }

  if (state == LocalNetworkState::GITHUB_NO_UPDATE || state == LocalNetworkState::GITHUB_FAILED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      startGithubUpdateCheck();
      return;
    }
    if (state == LocalNetworkState::GITHUB_NO_UPDATE && mappedInput.wasPressed(MenuNav::itemPrev()) &&
        !githubUpdater.getLatestVersion().empty()) {
      prepareGithubReleaseNotes();
      githubReleaseNotesScrollOffset = 0;
      githubReinstalling = true;
      state = LocalNetworkState::GITHUB_CONFIRMATION;
      updateRequired = true;
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      returnToUpdateServer();
    }
    return;
  }

  if (state == LocalNetworkState::WIFI_AUTO_CONNECTING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (onGoBack) onGoBack();
      return;
    }

    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      connectedSSID = WiFi.SSID().c_str();
      connectedIP = WiFi.localIP().toString().c_str();
      finishConnectedNetwork();
      return;
    }

    if (WiFi.status() == WL_CONNECT_FAILED || WiFi.status() == WL_NO_SSID_AVAIL ||
        millis() - wifiConnectionStartTime >= SAVED_WIFI_TIMEOUT_MS) {
      if (libraryLanding && onSwitchMode) {
        Serial.printf("[%lu] [LOCALNET] Saved WiFi unavailable; starting library hotspot\n", millis());
        WiFi.disconnect();
        onSwitchMode();
        return;
      }
      Serial.printf("[%lu] [LOCALNET] Saved WiFi unavailable; opening picker\n", millis());
      WiFi.disconnect();
      startWifiSelection();
    }
    return;
  }

  if (state == LocalNetworkState::SERVER_RUNNING && webServer && webServer->isRunning()) {
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 2000) {
      lastWifiCheck = millis();
      if (WiFi.status() != WL_CONNECTED) {
        if (libraryLanding && onSwitchMode) {
          Serial.printf("[%lu] [LOCALNET] Library WiFi disconnected; switching to hotspot\n", millis());
          onSwitchMode();
          return;
        }
        Serial.printf("[%lu] [LOCALNET] WiFi disconnected!\n", millis());
        stopWebServer();
        state = LocalNetworkState::ERROR;
        updateRequired = true;
        return;
      }
    }

    esp_task_wdt_reset();

    constexpr int MAX_ITERATIONS = 500;
    for (int i = 0; i < MAX_ITERATIONS && webServer->isRunning(); i++) {
      webServer->handleClient();
      if ((i & 0x1F) == 0x1F) {
        esp_task_wdt_reset();
      }
      if ((i & 0x3F) == 0x3F) {
        yield();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          Serial.printf("[%lu] [LOCALNET] Back button pressed\n", millis());
          if (onGoBack) onGoBack();
          return;
        }
        if (libraryLanding && mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
          Serial.printf("[%lu] [LOCALNET] Switching library server to hotspot\n", millis());
          if (onSwitchMode) onSwitchMode();
          return;
        }
      }
    }
  }

  if (state == LocalNetworkState::SERVER_RUNNING && updateLanding &&
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    startGithubUpdateCheck();
    return;
  }

  if (state == LocalNetworkState::SERVER_RUNNING && libraryLanding &&
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    Serial.printf("[%lu] [LOCALNET] Switching library server to hotspot\n", millis());
    if (onSwitchMode) onSwitchMode();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    Serial.printf("[%lu] [LOCALNET] Back button pressed\n", millis());
    if (onGoBack) onGoBack();
  }
}

/**
 * @brief Background task loop that handles display updates
 */
void LocalNetworkActivity::displayTaskLoop() {
  while (true) {
    bool shouldRender = updateRequired;
    int progressPercent = -1;
    if (state == LocalNetworkState::GITHUB_INSTALLING) {
      const size_t processed = githubUpdater.getProcessedSize();
      const size_t total = githubUpdater.getTotalSize();
      progressPercent = total > 0 ? std::min(100, static_cast<int>((processed * 100ULL) / total)) : 0;
      shouldRender = shouldRender || githubDisplayedPercent < 0 || progressPercent >= githubDisplayedPercent + 5 ||
                     (progressPercent == 100 && githubDisplayedPercent < 100);
    } else {
      githubDisplayedPercent = -1;
    }
    if (shouldRender) {
      updateRequired = false;
      if (progressPercent >= 0) githubDisplayedPercent = progressPercent;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

/**
 * @brief Main rendering function that dispatches to appropriate state renderers
 */
void LocalNetworkActivity::render() const {
  renderer.clearScreen();

  int screenHeight = renderer.getScreenHeight();
  int startY = 0;

  if (state == LocalNetworkState::SERVER_RUNNING) {
    renderServerRunning();
  } else if (state == LocalNetworkState::GITHUB_CHECKING) {
    const int contentStart = renderActivityHeader(renderer, startY, "GitHub Update");
    const int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY - 12, "Checking GitHub releases...", true,
                           EpdFontFamily::BOLD);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 24, "This may take a moment.");
  } else if (state == LocalNetworkState::GITHUB_CONFIRMATION) {
    const int bodyTop = renderActivityHeader(renderer, startY, "GitHub Update");
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 18, bodyTop + 2, "Current: " INX_VERSION, true);
    const std::string available = (githubReinstalling ? "Latest: " : "Available: ") +
                                  githubUpdater.getLatestVersion() + (githubReinstalling ? " (installed)" : "");
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 18, bodyTop + 29, available.c_str(), true,
                         EpdFontFamily::BOLD);
    renderer.line.render(18, bodyTop + 57, renderer.getScreenWidth() - 18, bodyTop + 57, true,
                         LineRender::Style::Dotted);

    const int firstLineY = bodyTop + GITHUB_NOTES_FIRST_OFFSET;
    const int notesBottom = screenHeight - 44;
    const int lineHeight = renderer.text.getLineHeight(GITHUB_NOTES_FONT) + 4;
    const int visibleLines = std::max(1, (notesBottom - firstLineY) / lineHeight);
    const int maxScroll = std::max(0, static_cast<int>(githubReleaseNoteLines.size()) - visibleLines);
    for (int i = 0;
         i < visibleLines && githubReleaseNotesScrollOffset + i < static_cast<int>(githubReleaseNoteLines.size());
         ++i) {
      const std::string& line = githubReleaseNoteLines[static_cast<size_t>(githubReleaseNotesScrollOffset + i)];
      if (!line.empty()) {
        renderer.text.render(GITHUB_NOTES_FONT, 18, firstLineY + i * lineHeight, line.c_str(), true);
      }
    }
    if (maxScroll > 0) {
      char position[20] = {};
      snprintf(position, sizeof(position), "%d/%d", githubReleaseNotesScrollOffset + 1, maxScroll + 1);
      const int positionWidth = renderer.text.getWidth(GITHUB_NOTES_FONT, position);
      renderer.text.render(GITHUB_NOTES_FONT, renderer.getScreenWidth() - positionWidth - 18, bodyTop + 59, position,
                           true);
    }
  } else if (state == LocalNetworkState::GITHUB_INSTALLING) {
    const int contentStart = renderActivityHeader(renderer, startY, "GitHub Update");
    const int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;
    const size_t processed = githubUpdater.getProcessedSize();
    const size_t total = githubUpdater.getTotalSize();
    const int percent = total > 0 ? std::min(100, static_cast<int>((processed * 100) / total)) : 0;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, centerY - 70, "Downloading + installing", true,
                           EpdFontFamily::BOLD);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY - 32,
                           "Keep the reader powered until it restarts.");
    const int barWidth = std::min(300, renderer.getScreenWidth() - 72);
    const int barX = (renderer.getScreenWidth() - barWidth) / 2;
    const int barY = centerY + 8;
    renderer.rectangle.render(barX, barY, barWidth, 6, true);
    if (percent > 0) {
      renderer.rectangle.fill(barX + 1, barY + 1, (barWidth - 2) * percent / 100, 4, true);
    }
    const std::string progress = std::to_string(percent) + "% - " + formatBytes(processed) + " / " +
                                 formatBytes(total);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, barY + 26, progress.c_str());
  } else if (state == LocalNetworkState::GITHUB_NO_UPDATE) {
    const int contentStart = renderActivityHeader(renderer, startY, "GitHub Update");
    const int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, centerY - 24, "Already up to date", true,
                           EpdFontFamily::BOLD);
    const std::string latest = "Latest release: " + githubUpdater.getLatestVersion();
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 20, latest.c_str());
    if (!githubUpdater.getLatestVersion().empty()) {
      renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 44,
                             "See Latest to review it, or reinstall it as it is.");
    }
  } else if (state == LocalNetworkState::GITHUB_FAILED) {
    const int contentStart = renderActivityHeader(renderer, startY, "GitHub Update");
    const int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, centerY - 24, "Update failed", true,
                           EpdFontFamily::BOLD);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 20, githubUpdateError.c_str());
  } else if (state == LocalNetworkState::GITHUB_FINISHED) {
    const int contentStart = renderActivityHeader(renderer, startY, "GitHub Update");
    const int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, centerY - 22, "Update installed", true,
                           EpdFontFamily::BOLD);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 20, "Restarting the reader...");
  } else if (state == LocalNetworkState::WIFI_AUTO_CONNECTING) {
    const int contentStart =
        renderActivityHeader(renderer, startY, updateLanding ? "Update Server" : (libraryLanding ? "Library Server" : "Local Network"));
    const int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY - 16, "Connecting to saved WiFi...");
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 20, truncateString(connectedSSID, 30).c_str());
  } else if (state == LocalNetworkState::SERVER_STARTING) {
    const int contentStart =
        renderActivityHeader(renderer, startY, updateLanding ? "Update Server" : (libraryLanding ? "Library Server" : "Local Network"));

    int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;

    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY, "Please wait...");
  } else if (state == LocalNetworkState::ERROR) {
    const int contentStart = renderActivityHeader(renderer, startY, "Local Network");

    int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;

    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY - 20, "Could not start server");
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY + 10, "Press Back to try again");
  }

  MappedInputManager::Labels labels;
  if (state == LocalNetworkState::SERVER_RUNNING && updateLanding) {
    labels = mappedInput.mapLabels("« Back", "GitHub Update", "", "");
  } else if (state == LocalNetworkState::SERVER_RUNNING && libraryLanding) {
    labels = mappedInput.mapLabels("« Back", "Hotspot", "", "");
  } else if (state == LocalNetworkState::GITHUB_CONFIRMATION) {
    labels = mappedInput.mapLabels("Cancel", githubReinstalling ? "Reinstall" : "Install", "Up", "Down");
  } else if (state == LocalNetworkState::GITHUB_NO_UPDATE || state == LocalNetworkState::GITHUB_FAILED) {
    // Back and Confirm are already spoken for by Server and Retry, so See
    // Latest takes the third of the X3's four front buttons — the one
    // mapLabels() hands to the "previous" slot.
    const bool canReinstall =
        state == LocalNetworkState::GITHUB_NO_UPDATE && !githubUpdater.getLatestVersion().empty();
    labels = mappedInput.mapLabels("« Server", "Retry", canReinstall ? "See Latest" : "", "");
  } else if (state == LocalNetworkState::GITHUB_CHECKING || state == LocalNetworkState::GITHUB_INSTALLING ||
             state == LocalNetworkState::GITHUB_FINISHED) {
    labels = mappedInput.mapLabels("", "", "", "");
  } else {
    labels = mappedInput.mapLabels("« Back", "", "", "");
  }
  if (state == LocalNetworkState::SERVER_RUNNING && (updateLanding || libraryLanding)) {
    renderer.ui.buttonHintsFit(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}

/**
 * @brief Renders the server running state UI with connection information
 */
void LocalNetworkActivity::renderServerRunning() const {
  int startY = 0;

  const int contentStart =
      renderActivityHeader(renderer, startY, updateLanding ? "Update Server" : (libraryLanding ? "Library Server" : "Local Network"));

  const char* path = updateLanding ? "/update" : (libraryLanding ? "/library" : "/");
  std::string ipUrl = "http://" + connectedIP + path;
  // Built from the setting, not a constant: showing xteink.local while mDNS answers to
  // something else leaves no way to discover the address the reader actually has.
  std::string hostnameUrl = std::string("http://") + SETTINGS.getDeviceHostname() + ".local" + path;

  const int bodyTop = contentStart + 56;
  const int labelFont = ATKINSON_HYPERLEGIBLE_8_FONT_ID;
  const int titleFont = ATKINSON_HYPERLEGIBLE_14_FONT_ID;
  const int bodyFont = ATKINSON_HYPERLEGIBLE_10_FONT_ID;

  renderer.text.centered(labelFont, bodyTop, updateLanding ? "FIRMWARE UPDATE" : (libraryLanding ? "LIBRARY MANAGER" : "LOCAL TRANSFER"), true,
                         EpdFontFamily::BOLD);
  renderer.text.centered(titleFont, bodyTop + 34, updateLanding ? "Update server ready" : (libraryLanding ? "Library ready" : "Ready on WiFi"), true,
                         EpdFontFamily::BOLD);
  renderer.text.centered(bodyFont, bodyTop + 74, truncateString(connectedSSID, 30).c_str());

  const int urlY = bodyTop + 136;
  renderer.text.centered(labelFont, urlY, "OPEN IN BROWSER", true, EpdFontFamily::BOLD);
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_12_FONT_ID, urlY + 32, ipUrl.c_str(), true, EpdFontFamily::BOLD);
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, urlY + 64, hostnameUrl.c_str());

  const int hintY = renderer.getScreenHeight() - 92;
  renderer.text.centered(
      ATKINSON_HYPERLEGIBLE_8_FONT_ID, hintY,
      updateLanding ? "Keep this screen open during the update"
                    : (libraryLanding ? "Keep this screen open while managing books"
                                      : "Keep this screen open while transferring"));
}
