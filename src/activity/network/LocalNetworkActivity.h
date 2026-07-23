#pragma once

/**
 * @file LocalNetworkActivity.h
 * @brief Public interface and types for LocalNetworkActivity.
 */

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>
#include <string>

#include "WifiSelectionActivity.h"
#include "activity/ActivityWithSubactivity.h"
#include "activity/Menu.h"
#include "network/LocalServer.h"

/**
 * @brief Represents the operational states of the local network activity
 */
enum class LocalNetworkState {
  WIFI_AUTO_CONNECTING, /**< Connecting to the most recently saved network */
  WIFI_SELECTION,  /**< Waiting for WiFi network selection */
  SERVER_STARTING, /**< Web server initialization in progress */
  SERVER_RUNNING,  /**< Web server active and accepting connections */
  ERROR            /**< Error state, unable to proceed */
};

/**
 * @brief Activity that enables file transfer over local WiFi network
 *
 * This activity connects to an existing WiFi network and starts a web server
 * for file transfers. It handles WiFi connection setup, server initialization,
 * and provides connection information to users.
 */
class LocalNetworkActivity final : public ActivityWithSubactivity, public Menu {
 public:
  /**
   * @brief Constructs a new LocalNetworkActivity
   * @param renderer Graphics renderer for display output
   * @param mappedInput Input manager for handling user interactions
   * @param onGoBack Callback function invoked when user requests to go back
   */
  explicit LocalNetworkActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::function<void()>& onGoBack, bool autoConnectSaved = false,
                                bool updateLanding = false)
      : ActivityWithSubactivity("LocalNetwork", renderer, mappedInput),
        Menu(),
        displayTaskHandle(nullptr),
        renderingMutex(nullptr),
        updateRequired(false),
        state(LocalNetworkState::WIFI_SELECTION),
        wifiSelectionCompletionPending(false),
        wifiSelectionConnected(false),
        autoConnectSaved(autoConnectSaved),
        updateLanding(updateLanding),
        wifiConnectionStartTime(0),
        lastHandleClientTime(0),
        onGoBack(onGoBack) {
    tabSelectorIndex = 4;
  }

  /**
   * @brief Called when activity becomes active
   * @details Initializes WiFi connection or starts web server
   */
  void onEnter() override;

  /**
   * @brief Called when activity is exited
   * @details Stops web server and cleans up resources
   */
  void onExit() override;

  /** @brief Main loop - processes network connections */
  void loop() override;

  /**
   * @brief Determines if loop delay should be skipped
   * @return true when server is running to maintain responsiveness
   */
  bool skipLoopDelay() override { return state == LocalNetworkState::SERVER_RUNNING; }

  /**
   * @brief Prevents auto sleep during active connections
   * @return true when server is running to maintain connection
   */
  bool preventAutoSleep() override { return state == LocalNetworkState::SERVER_RUNNING; }

 private:
  /**
   * @brief Static trampoline function for FreeRTOS task
   * @param param Pointer to LocalNetworkActivity instance
   */
  static void taskTrampoline(void* param);

  /**
   * @brief Background task loop for display updates
   * @details Never returns - [[noreturn]] attribute
   */
  [[noreturn]] void displayTaskLoop();

  /** @brief Main rendering function */
  void render() const;

  /** @brief Renders the server running state UI */
  void renderServerRunning() const;

  /**
   * @brief Callback for WiFi selection completion
   * @param connected True if WiFi connection successful
   */
  void onWifiSelectionComplete(bool connected);

  /** @brief Completes the WiFi handoff after the child callback has returned */
  void finishWifiSelection();

  /** @brief Connects to the most recently saved WiFi network */
  void startSavedWifiConnection();

  /** @brief Opens the normal WiFi picker */
  void startWifiSelection();

  /** @brief Starts services after either automatic or manual WiFi connection */
  void finishConnectedNetwork();

  /** @brief Initializes and starts the web server */
  void startWebServer();

  /** @brief Stops the web server and cleans up resources */
  void stopWebServer();

  /** @brief Navigate to selected menu tab (not used in this activity) */
  void navigateToSelectedMenu() override {}

  TaskHandle_t displayTaskHandle;   /**< Handle for display update task */
  SemaphoreHandle_t renderingMutex; /**< Mutex for thread-safe rendering */
  bool updateRequired;              /**< Flag indicating render update needed */
  LocalNetworkState state;          /**< Current activity state */
  bool wifiSelectionCompletionPending; /**< Deferred child completion callback */
  bool wifiSelectionConnected;          /**< Result captured by the deferred callback */
  const bool autoConnectSaved;           /**< Try the newest saved credential before showing the picker */
  const bool updateLanding;              /**< Show the /update URL and update-specific device copy */
  unsigned long wifiConnectionStartTime; /**< Start time for saved-network connection timeout */

  std::string connectedIP;                /**< IP address of connected WiFi */
  std::string connectedSSID;              /**< SSID of connected WiFi network */
  std::unique_ptr<LocalServer> webServer; /**< Web server instance */
  unsigned long lastHandleClientTime;     /**< Timestamp of last client handling */

  const std::function<void()> onGoBack; /**< Callback invoked when going back */
};
