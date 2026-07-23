/**
 * @file LocalNetworkActivity.cpp
 * @brief Definitions for LocalNetworkActivity.
 */

#include "LocalNetworkActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include "WifiSelectionActivity.h"
#include "state/NetworkCredential.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/ScreenComponents.h"
#include "system/UiTheme.h"

namespace {
constexpr const char* AP_HOSTNAME = "xteink";

constexpr int CONTENT_MARGIN = 25;
constexpr int LINE_SPACING = 28;
constexpr int SMALL_SPACING = 25;
constexpr int SECTION_SPACING = 40;
constexpr int BOTTOM_AREA_HEIGHT = 80;
constexpr unsigned long SAVED_WIFI_TIMEOUT_MS = 15000;

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

  if (MDNS.begin(AP_HOSTNAME)) {
    Serial.printf("[%lu] [LOCALNET] mDNS started: http://%s.local/\n", millis(), AP_HOSTNAME);
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
      }
    }
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
    if (updateRequired) {
      updateRequired = false;
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
  } else if (state == LocalNetworkState::WIFI_AUTO_CONNECTING) {
    const int contentStart = renderActivityHeader(renderer, startY, updateLanding ? "Update Server" : "Local Network");
    const int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY - 16, "Connecting to saved WiFi...");
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, centerY + 20,
                           truncateString(connectedSSID, 30).c_str());
  } else if (state == LocalNetworkState::SERVER_STARTING) {
    const int contentStart = renderActivityHeader(renderer, startY, updateLanding ? "Update Server" : "Local Network");

    int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;

    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY, "Please wait...");
  } else if (state == LocalNetworkState::ERROR) {
    const int contentStart = renderActivityHeader(renderer, startY, "Local Network");

    int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;

    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY - 20, "Could not start server");
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY + 10, "Press Back to try again");
  }

  auto labels = mappedInput.mapLabels("« Back", "", "", "");
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

/**
 * @brief Renders the server running state UI with connection information
 */
void LocalNetworkActivity::renderServerRunning() const {
  int startY = 0;

  const int contentStart = renderActivityHeader(renderer, startY, updateLanding ? "Update Server" : "Local Network");

  const char* path = updateLanding ? "/update" : "/";
  std::string ipUrl = "http://" + connectedIP + path;
  std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local" + path;

  const int bodyTop = contentStart + 56;
  const int labelFont = ATKINSON_HYPERLEGIBLE_8_FONT_ID;
  const int titleFont = ATKINSON_HYPERLEGIBLE_14_FONT_ID;
  const int bodyFont = ATKINSON_HYPERLEGIBLE_10_FONT_ID;

  renderer.text.centered(labelFont, bodyTop, updateLanding ? "FIRMWARE UPDATE" : "LOCAL TRANSFER", true,
                         EpdFontFamily::BOLD);
  renderer.text.centered(titleFont, bodyTop + 34, updateLanding ? "Update server ready" : "Ready on WiFi", true,
                         EpdFontFamily::BOLD);
  renderer.text.centered(bodyFont, bodyTop + 74, truncateString(connectedSSID, 30).c_str());

  const int urlY = bodyTop + 136;
  renderer.text.centered(labelFont, urlY, "OPEN IN BROWSER", true, EpdFontFamily::BOLD);
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_12_FONT_ID, urlY + 32, ipUrl.c_str(), true, EpdFontFamily::BOLD);
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, urlY + 64, hostnameUrl.c_str());

  const int hintY = renderer.getScreenHeight() - 92;
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, hintY,
                         updateLanding ? "Keep this screen open during the update"
                                       : "Keep this screen open while transferring");
}
