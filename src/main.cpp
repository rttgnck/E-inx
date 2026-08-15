/**
 * @file main.cpp
 * @brief Firmware entry point, globals, and activity bootstrap.
 */

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <Preferences.h>
#include <SDCardManager.h>
#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>
#include <esp_system.h>
#ifndef SIMULATOR
#include <esp_ota_ops.h>
#endif

#include <Epub.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <new>
#include <string>

#include "activity/OpdsServerListActivity.h"
#include "activity/network/BluetoothTransferActivity.h"
#include "activity/network/CalibreConnectActivity.h"
#include "activity/network/HotspotActivity.h"
#include "activity/network/LocalNetworkActivity.h"
#include "activity/games/AppsActivity.h"
#include "activity/games/GamesActivity.h"
#include "agentisland/AgentIslandActivity.h"
#include "crossplay/CrossPlayActivity.h"
#include "activity/page/LibraryActivity.h"
#include "activity/page/NewsActivity.h"
#include "activity/page/RecentActivity.h"
#include "activity/page/SettingsActivity.h"
#include "activity/page/StatisticActivity.h"
#include "activity/page/SyncActivity.h"
#include "activity/reader/ImageViewerActivity.h"
#include "activity/reader/ReaderActivity.h"
#include "activity/settings/EditMetadataActivity.h"
#include "activity/system/BootActivity.h"
#include "activity/system/SleepActivity.h"
#include "activity/util/FullScreenMessageActivity.h"
#include "state/BookState.h"
#include "state/OpdsServerStore.h"
#include "state/RecentBooks.h"
#include "state/Session.h"
#include "state/SystemSetting.h"
#include "system/FontManager.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/SleepWakeTraceStore.h"
#include "util/StringUtils.h"

#ifdef SIMULATOR
extern HalDisplay display;
extern HalGPIO gpio;
#else
HalDisplay display;
HalGPIO gpio;
#endif
MappedInputManager input(gpio);
GfxRenderer renderer(display);
GfxRenderer& render = renderer;

Activity* currentActivity = nullptr;

unsigned long t1 = 0;
unsigned long t2 = 0;
unsigned long lastActivityTime = 0;
unsigned long ignorePowerSleepUntil = 0;
RTC_DATA_ATTR bool rtcLastSleepPowerDoublePressEnabled = false;
RTC_DATA_ATTR bool rtcLastSleepSleptFromReader = false;
RTC_DATA_ATTR char rtcLastReadPath[256] = {};
RTC_DATA_ATTR uint16_t rtcLastSleepPowerWakeGuardMs = 0;
RTC_DATA_ATTR uint8_t rtcLastSleepPowerGestureWindow = 0;
RTC_DATA_ATTR uint8_t rtcLastSleepPowerFirstPressMin = 0;
RTC_DATA_ATTR uint8_t rtcLastSleepPowerSecondPressMax = 2;
RTC_DATA_ATTR uint32_t rtcEarlyWakeConfigSignature = 0;
RTC_DATA_ATTR uint32_t rtcPersistedEarlyWakeConfig = 0;
RTC_DATA_ATTR uint32_t rtcEarlyGestureConfigSignature = 0;
RTC_DATA_ATTR uint32_t rtcPersistedEarlyGestureConfig = 0;
RTC_DATA_ATTR uint32_t rtcExpectedSleepTimerWakeSignature = 0;

namespace {
constexpr unsigned long WAKE_POWER_GUARD_MS = 1500;
constexpr unsigned long POWER_WAKE_EMERGENCY_BOOT_MS = 3000;
constexpr unsigned long POWER_DOUBLE_PRESS_NO_GUARD_WINDOW_MS = 650;
constexpr unsigned long POWER_DOUBLE_PRESS_GUARD_MARGIN_MS = 300;
constexpr unsigned long POWER_DOUBLE_PRESS_DEFAULT_SECOND_HOLD_MAX_MS = 300;
constexpr uint32_t MAX_SLEEP_IMAGE_TIMER_SECONDS = 120UL * 60UL;
constexpr uint32_t EXPECTED_SLEEP_TIMER_WAKE_SIGNATURE = 0x4958544DUL;
constexpr uint32_t EARLY_WAKE_CONFIG_SIGNATURE = 0x49580000UL;
constexpr uint32_t EARLY_WAKE_CONFIG_SIGNATURE_MASK = 0xFFF80000UL;
constexpr uint32_t EARLY_WAKE_CONFIG_GUARD_MASK = 0x00000FFFUL;
constexpr uint32_t EARLY_WAKE_CONFIG_DOUBLE_PRESS = 1UL << 12;
constexpr uint32_t EARLY_WAKE_CONFIG_SLEPT_FROM_READER = 1UL << 13;
constexpr uint32_t EARLY_WAKE_CONFIG_GESTURE_SHIFT = 14;
constexpr uint32_t EARLY_WAKE_CONFIG_GESTURE_MASK = 0x0007C000UL;
constexpr uint32_t EARLY_GESTURE_CONFIG_SIGNATURE = 0x49584700UL;
constexpr uint32_t EARLY_GESTURE_CONFIG_SIGNATURE_MASK = 0xFFFFFF00UL;
constexpr uint32_t EARLY_GESTURE_CONFIG_FIRST_PRESS_MASK = 0x0000000FUL;
constexpr uint32_t EARLY_GESTURE_CONFIG_SECOND_PRESS_SHIFT = 4;
constexpr uint32_t EARLY_GESTURE_CONFIG_SECOND_PRESS_MASK = 0x000000F0UL;
constexpr char EARLY_WAKE_NVS_NAMESPACE[] = "inxwake";
constexpr char EARLY_WAKE_NVS_KEY[] = "config";
constexpr char EARLY_GESTURE_NVS_KEY[] = "gesture";

enum class PowerWakeAction { Boot, ShortPress, DoublePress };

bool startupPowerWakeDecisionReady = false;
bool startupPowerWakeCandidate = false;
bool startupTimerWakeCandidate = false;
PowerWakeAction startupPowerWakeAction = PowerWakeAction::Boot;

bool timeBefore(const unsigned long a, const unsigned long b) { return static_cast<long>(a - b) < 0; }

void confirmBootedOtaImage() {
#ifndef SIMULATOR
  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (runningPartition == nullptr || esp_ota_get_state_partition(runningPartition, &state) != ESP_OK ||
      state != ESP_OTA_IMG_PENDING_VERIFY) {
    return;
  }

  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  Serial.printf("[%lu] [OTA] First-boot validation for %s: %s\n", millis(), runningPartition->label,
                esp_err_to_name(err));
#endif
}

bool endsWithIgnoreCase(const std::string& value, const char* suffix) {
  const size_t suffixLen = strlen(suffix);
  if (value.size() < suffixLen) {
    return false;
  }
  const size_t start = value.size() - suffixLen;
  for (size_t i = 0; i < suffixLen; ++i) {
    const auto a = static_cast<unsigned char>(value[start + i]);
    const auto b = static_cast<unsigned char>(suffix[i]);
    if (std::tolower(a) != std::tolower(b)) {
      return false;
    }
  }
  return true;
}

bool earlyPowerWakeCandidate() {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();
  return wakeupCause == ESP_SLEEP_WAKEUP_GPIO ||
         (wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON);
}

bool powerButtonHeldStableAtStartup() {
  constexpr uint8_t POWER_BUTTON_GPIO = 3;
  constexpr unsigned long STABLE_HOLD_SAMPLE_MS = 20;
  pinMode(POWER_BUTTON_GPIO, INPUT_PULLUP);
  const bool firstSampleLow = digitalRead(POWER_BUTTON_GPIO) == LOW;
  if (!firstSampleLow) {
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::PowerSample, 0, 0);
    return false;
  }
  delay(STABLE_HOLD_SAMPLE_MS);
  const bool secondSampleLow = digitalRead(POWER_BUTTON_GPIO) == LOW;
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::PowerSample, 1, secondSampleLow ? 1 : 0);
  return secondSampleLow;
}

bool earlyTimerWakeCandidate() {
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();
  const uint32_t stubCause = HalGPIO::getLastWakeStubCause();
  HalGPIO::recordSleepWakeTrace(
      HalGPIO::SleepWakeTraceEvent::EarlyWake,
      (static_cast<uint32_t>(resetReason) << 16) | (static_cast<uint32_t>(wakeupCause) & 0xFFFFUL), stubCause);

  if (wakeupCause == ESP_SLEEP_WAKEUP_TIMER) {
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::ClassifiedTimer, 1, stubCause);
    return true;
  }
  if (HalGPIO::lastWakeStubWasTimer()) {
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::ClassifiedTimer, 2, stubCause);
    return true;
  }

  const bool signatureValid = rtcExpectedSleepTimerWakeSignature == EXPECTED_SLEEP_TIMER_WAKE_SIGNATURE;
  const HalGPIO::SleepTimerTickState timing = HalGPIO::getSleepTimerTickState();
  const uint64_t elapsedTicks = timing.currentTicks - timing.startTicks;
  const bool deadlineReached = timing.durationTicks != 0 && elapsedTicks >= timing.durationTicks;
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::DeadlineCheck, static_cast<uint32_t>(elapsedTicks),
                                static_cast<uint32_t>(timing.durationTicks) | (signatureValid ? 0x80000000UL : 0));
  if (!signatureValid || !deadlineReached) {
    return false;
  }

  // The X3 can expose an automatic wake through an ambiguous GPIO/undefined cause. A Power key that remains
  // physically held still wins, preserving the configured wake guard and unconditional three-second escape.
  if (powerButtonHeldStableAtStartup()) {
    return false;
  }
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::ClassifiedTimer, 3, stubCause);
  return true;
}

uint32_t packEarlyWakeConfig(const uint16_t guardMs, const bool doublePressEnabled, const bool sleptFromReader,
                             const uint8_t gestureWindow) {
  return EARLY_WAKE_CONFIG_SIGNATURE | (static_cast<uint32_t>(guardMs) & EARLY_WAKE_CONFIG_GUARD_MASK) |
         (doublePressEnabled ? EARLY_WAKE_CONFIG_DOUBLE_PRESS : 0) |
         (sleptFromReader ? EARLY_WAKE_CONFIG_SLEPT_FROM_READER : 0) |
         ((static_cast<uint32_t>(gestureWindow) << EARLY_WAKE_CONFIG_GESTURE_SHIFT) & EARLY_WAKE_CONFIG_GESTURE_MASK);
}

bool validEarlyWakeConfig(const uint32_t packed) {
  return (packed & EARLY_WAKE_CONFIG_SIGNATURE_MASK) == EARLY_WAKE_CONFIG_SIGNATURE &&
         (packed & EARLY_WAKE_CONFIG_GUARD_MASK) <= 2500 &&
         ((packed & EARLY_WAKE_CONFIG_GESTURE_MASK) >> EARLY_WAKE_CONFIG_GESTURE_SHIFT) <= 17;
}

uint32_t packEarlyGestureConfig(const uint8_t firstPressMin, const uint8_t secondPressMax) {
  return EARLY_GESTURE_CONFIG_SIGNATURE |
         (static_cast<uint32_t>(firstPressMin) & EARLY_GESTURE_CONFIG_FIRST_PRESS_MASK) |
         ((static_cast<uint32_t>(secondPressMax) << EARLY_GESTURE_CONFIG_SECOND_PRESS_SHIFT) &
          EARLY_GESTURE_CONFIG_SECOND_PRESS_MASK);
}

bool validEarlyGestureConfig(const uint32_t packed) {
  const uint8_t firstPressMin = static_cast<uint8_t>(packed & EARLY_GESTURE_CONFIG_FIRST_PRESS_MASK);
  const uint8_t secondPressMax = static_cast<uint8_t>((packed & EARLY_GESTURE_CONFIG_SECOND_PRESS_MASK) >>
                                                      EARLY_GESTURE_CONFIG_SECOND_PRESS_SHIFT);
  return (packed & EARLY_GESTURE_CONFIG_SIGNATURE_MASK) == EARLY_GESTURE_CONFIG_SIGNATURE && firstPressMin <= 15 &&
         secondPressMax <= 9;
}

void applyEarlyWakeConfig(const uint32_t packed) {
  rtcLastSleepPowerWakeGuardMs = static_cast<uint16_t>(packed & EARLY_WAKE_CONFIG_GUARD_MASK);
  rtcLastSleepPowerDoublePressEnabled = (packed & EARLY_WAKE_CONFIG_DOUBLE_PRESS) != 0;
  rtcLastSleepSleptFromReader = (packed & EARLY_WAKE_CONFIG_SLEPT_FROM_READER) != 0;
  rtcLastSleepPowerGestureWindow =
      static_cast<uint8_t>((packed & EARLY_WAKE_CONFIG_GESTURE_MASK) >> EARLY_WAKE_CONFIG_GESTURE_SHIFT);
  rtcEarlyWakeConfigSignature = EARLY_WAKE_CONFIG_SIGNATURE;
  rtcPersistedEarlyWakeConfig = packed;
}

void applyEarlyGestureConfig(const uint32_t packed) {
  rtcLastSleepPowerFirstPressMin = static_cast<uint8_t>(packed & EARLY_GESTURE_CONFIG_FIRST_PRESS_MASK);
  rtcLastSleepPowerSecondPressMax = static_cast<uint8_t>((packed & EARLY_GESTURE_CONFIG_SECOND_PRESS_MASK) >>
                                                         EARLY_GESTURE_CONFIG_SECOND_PRESS_SHIFT);
  rtcEarlyGestureConfigSignature = EARLY_GESTURE_CONFIG_SIGNATURE;
  rtcPersistedEarlyGestureConfig = packed;
}

void loadEarlyWakeConfig() {
  const bool wakeConfigReady = rtcEarlyWakeConfigSignature == EARLY_WAKE_CONFIG_SIGNATURE;
  const bool gestureConfigReady = rtcEarlyGestureConfigSignature == EARLY_GESTURE_CONFIG_SIGNATURE;
  if (wakeConfigReady && gestureConfigReady) {
    return;
  }

  Preferences prefs;
  uint32_t packed = 0;
  uint32_t gesturePacked = 0;
  if (prefs.begin(EARLY_WAKE_NVS_NAMESPACE, true)) {
    if (!wakeConfigReady) packed = prefs.getUInt(EARLY_WAKE_NVS_KEY, 0);
    if (!gestureConfigReady) gesturePacked = prefs.getUInt(EARLY_GESTURE_NVS_KEY, 0);
    prefs.end();
  }
  if (!wakeConfigReady) {
    if (validEarlyWakeConfig(packed)) {
      applyEarlyWakeConfig(packed);
    } else {
      rtcLastSleepPowerWakeGuardMs = 0;
      rtcLastSleepPowerDoublePressEnabled = false;
      rtcLastSleepSleptFromReader = false;
      rtcLastSleepPowerGestureWindow = 0;
      rtcEarlyWakeConfigSignature = EARLY_WAKE_CONFIG_SIGNATURE;
      rtcPersistedEarlyWakeConfig = 0;
    }
  }
  if (!gestureConfigReady) {
    if (validEarlyGestureConfig(gesturePacked)) {
      applyEarlyGestureConfig(gesturePacked);
    } else {
      rtcLastSleepPowerFirstPressMin = 0;
      rtcLastSleepPowerSecondPressMax = 2;
      rtcEarlyGestureConfigSignature = EARLY_GESTURE_CONFIG_SIGNATURE;
      rtcPersistedEarlyGestureConfig = 0;
    }
  }
}

void persistEarlyWakeConfig(const bool sleptFromReader) {
  const uint16_t guardMs = SETTINGS.getPowerWakeGuardMs();
  const bool doublePressEnabled = SleepActivity::shouldEnablePowerDoublePressImageAdvance(sleptFromReader);
  const uint8_t gestureWindow = SETTINGS.sleepImagePowerGestureWindow;
  const uint8_t firstPressMin = SETTINGS.sleepImagePowerFirstPressMin;
  const uint8_t secondPressMax = SETTINGS.sleepImagePowerSecondPressMax;
  const uint32_t packed = packEarlyWakeConfig(guardMs, doublePressEnabled, sleptFromReader, gestureWindow);
  const uint32_t gesturePacked = packEarlyGestureConfig(firstPressMin, secondPressMax);
  const bool persistWakeConfig = rtcPersistedEarlyWakeConfig != packed;
  const bool persistGestureConfig = rtcPersistedEarlyGestureConfig != gesturePacked;

  rtcLastSleepPowerWakeGuardMs = guardMs;
  rtcLastSleepPowerDoublePressEnabled = doublePressEnabled;
  rtcLastSleepSleptFromReader = sleptFromReader;
  rtcLastSleepPowerGestureWindow = gestureWindow;
  rtcLastSleepPowerFirstPressMin = firstPressMin;
  rtcLastSleepPowerSecondPressMax = secondPressMax;
  rtcEarlyWakeConfigSignature = EARLY_WAKE_CONFIG_SIGNATURE;
  rtcEarlyGestureConfigSignature = EARLY_GESTURE_CONFIG_SIGNATURE;
  if (!persistWakeConfig && !persistGestureConfig) {
    return;
  }

  Preferences prefs;
  if (prefs.begin(EARLY_WAKE_NVS_NAMESPACE, false)) {
    if (persistWakeConfig && prefs.putUInt(EARLY_WAKE_NVS_KEY, packed) == sizeof(packed)) {
      rtcPersistedEarlyWakeConfig = packed;
    }
    if (persistGestureConfig && prefs.putUInt(EARLY_GESTURE_NVS_KEY, gesturePacked) == sizeof(gesturePacked)) {
      rtcPersistedEarlyGestureConfig = gesturePacked;
    }
    prefs.end();
  }
}

}  // namespace

void waitForPowerRelease();
PowerWakeAction detectPowerWakeAction(bool powerWakeCandidate);
bool shouldUsePowerDoublePressForSleepImageAdvance(PowerWakeAction action);
bool shouldReSleepForShortPowerWake(PowerWakeAction action);
uint8_t wakeReasonCode(HalGPIO::WakeupReason wakeupReason);
void markActivityNow();
bool powerSleepGuardActive();
void normalizeUnavailableClockSettings();
void enterDeepSleep(bool forceSleepImageAdvance = false, bool continueExistingSleep = false);
void onGoToReader(const std::string& path);
void onGoToReaderNavigation(const std::string& path);
void onSelectBook(const std::string& path);
void onSelectBookNavigation(const std::string& path);
void onGoToRecent();
void onGoToNews();
void onGoToTab2();
void onGoToGames();
void onGoToApps();
void onGoToCrossPlay();
void onGoToAgentIsland();
void onGoToStatistics();
void onGoToFileTransfer();
void onGoToLibraryServer();
void onGoToLibraryHotspot();
void onGoToSettings();
void onGoToLibrary(const std::string& path = "/");
void onEditBookMetadata(const std::string& bookPath, const std::string& returnPath);
void setupDisplayAndFonts();
void onNetworkModeSelected(NetworkMode mode);
void openReaderFromCallback(const std::string& path);
bool handleGlobalPowerRefresh();

/**
 * @brief Switches the current activity using standard heap allocation.
 * * This uses 'new' and 'delete' which allows the ReaderActivity to utilize
 * the full 360KB of available heap rather than being stuck in a small static buffer.
 */
template <typename T, typename... Args>
T* switchTo(Args&&... args) {
  const bool hadActivity = currentActivity != nullptr;
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }

  if (hadActivity && SETTINGS.antiGhostingExperimental && !SETTINGS.darkMode) {
    render.requestNextHalfRefresh();
  }

  T* nextActivity = new T(std::forward<Args>(args)...);
  currentActivity = nextActivity;
#ifdef SIMULATOR
  Serial.printf("[%lu] [SIM] Activity: %s\n", millis(), currentActivity->getName());
#endif
  currentActivity->onEnter();
  // After the first paint, not before it: onEnter() blocks on a full e-ink refresh without
  // sampling the buttons, so the press that opened this screen is only finished settling by
  // the time it returns. Suppressing from here means the screen starts from a clean edge.
  input.ignoreInputUntilIdle();
  return nextActivity;
}

/**
 * @brief Navigates to the reader activity for a specific book.
 */
void onGoToReader(const std::string& path) {
  switchTo<ReaderActivity>(render, input, path, [](const std::string&) { onGoToRecent(); });
}

bool isExportedNoteImage(const std::string& path) {
  constexpr const char* root = "/Bookmarks & Annotations";
  const size_t rootLen = strlen(root);
  const bool inRoot = path.compare(0, rootLen, root) == 0 && (path.size() == rootLen || path[rootLen] == '/');
  return inRoot && (StringUtils::checkFileExtension(path, ".bmp") || StringUtils::checkFileExtension(path, ".jpg") ||
                    StringUtils::checkFileExtension(path, ".jpeg") || StringUtils::checkFileExtension(path, ".png"));
}

void onGoToReaderNavigation(const std::string& path) {
  switchTo<ReaderActivity>(
      render, input, path, [](const std::string&) { onGoToRecent(); },
      /*openNavigationOnLaunch=*/true);
}

/**
 * @brief Opens the reader activity and returns to the library when closed.
 */
void openReaderFromCallback(const std::string& path) {
  if (isExportedNoteImage(path)) {
    switchTo<ImageViewerActivity>(render, input, path, [path]() {
      std::string folderPath = path.substr(0, path.find_last_of('/'));
      if (folderPath.empty()) folderPath = "/";
      onGoToLibrary(folderPath);
    });
    return;
  }
  switchTo<ReaderActivity>(render, input, path, [path](const std::string&) {
    std::string folderPath = path.substr(0, path.find_last_of('/'));
    if (folderPath.empty()) folderPath = "/";
    onGoToLibrary(folderPath);
  });
}

/**
 * @brief Callback wrapper for selecting a book to read.
 */
void onSelectBook(const std::string& path) { onGoToReader(path); }

void onSelectBookNavigation(const std::string& path) { onGoToReaderNavigation(path); }

/**
 * @brief Navigates to the statistics activity.
 */
void onGoToStatistics() { switchTo<StatisticActivity>(render, input, onGoToRecent, onGoToFileTransfer); }

/**
 * @brief Navigates to the recent books activity.
 */
void onGoToNews() {
  switchTo<NewsActivity>(render, input, onGoToRecent, []() { onGoToLibrary("/"); }, onGoToReader, onGoToNews);
}

void onGoToGames() {
  switchTo<GamesActivity>(render, input, onGoToRecent, []() { onGoToLibrary("/"); });
}

void onGoToCrossPlay() {
  switchTo<CrossPlayActivityMenu>(render, input, onGoToRecent, []() { onGoToLibrary("/"); });
}

void onGoToAgentIsland() { switchTo<AgentIslandActivity>(render, input, onGoToApps); }

void onGoToApps() {
  switchTo<AppsActivity>(render, input, onGoToRecent, []() { onGoToLibrary("/"); }, onGoToNews, onGoToGames,
                         onGoToCrossPlay, onGoToAgentIsland);
}

void onGoToTab2() {
  switch (SETTINGS.tab2Content) {
    case SystemSetting::TAB2_GAMES:
      onGoToGames();
      break;
    case SystemSetting::TAB2_APPS:
      onGoToApps();
      break;
    default:
      onGoToNews();
      break;
  }
}

void onGoToRecent() {
  switchTo<RecentActivity>(
      render, input, onGoToTab2, []() { onGoToLibrary("/"); }, onGoToStatistics, onSelectBook, onSelectBookNavigation,
      onGoToRecent);
}

void onGoToLibraryServer() {
  switchTo<LocalNetworkActivity>(render, input, onGoToFileTransfer, true, false, true, onGoToLibraryHotspot);
}

void onGoToLibraryHotspot() {
  switchTo<HotspotActivity>(render, input, onGoToFileTransfer, true, onGoToLibraryServer);
}

/**
 * @brief Handles network mode selection and navigates to appropriate activity.
 */
void onNetworkModeSelected(NetworkMode mode) {
  switch (mode) {
    case NetworkMode::JOIN_NETWORK:
      switchTo<LocalNetworkActivity>(render, input, onGoToFileTransfer);
      break;
    case NetworkMode::UPDATE_SERVER:
      switchTo<LocalNetworkActivity>(render, input, onGoToFileTransfer, true, true);
      break;
    case NetworkMode::LIBRARY_SERVER:
      onGoToLibraryServer();
      break;
    case NetworkMode::BLUETOOTH_TRANSFER:
      switchTo<BluetoothTransferActivity>(render, input, onGoToFileTransfer);
      break;
    case NetworkMode::CONNECT_CALIBRE:
      switchTo<CalibreConnectActivity>(render, input, onGoToFileTransfer);
      break;
    case NetworkMode::CREATE_HOTSPOT:
      switchTo<HotspotActivity>(render, input, onGoToFileTransfer);
      break;
    case NetworkMode::OPDS_BROWSER:
      switchTo<OpdsServerListActivity>(render, input, onGoToFileTransfer);
      break;
  }
}

/**
 * @brief Navigates to the file transfer/sync activity.
 */
void onGoToFileTransfer() {
  switchTo<SyncActivity>(render, input, onNetworkModeSelected, onGoToRecent, onGoToStatistics, onGoToSettings);
}

/**
 * @brief Navigates to the settings activity.
 */
void onGoToSettings() {
  switchTo<SettingsActivity>(
      render, input, onGoToRecent, []() { onGoToLibrary("/"); }, onGoToFileTransfer, onGoToStatistics);
}

/**
 * @brief Navigates to the library activity.
 */
void onGoToLibrary(const std::string& path) {
  switchTo<LibraryActivity>(render, input, onGoToRecent, openReaderFromCallback, onGoToRecent, onGoToTab2,
                            onGoToSettings, onEditBookMetadata, path);
}

void onEditBookMetadata(const std::string& bookPath, const std::string& returnPath) {
  std::string title;
  std::string author;
  std::string language;
  bool favorite = false;
  if (const BookState::Book* b = BOOK_STATE.findBookByPath(bookPath)) {
    title = b->title;
    author = b->author;
    favorite = b->isFavorite;
  }
  for (const auto& recent : RECENT_BOOKS.getBooks()) {
    if (recent.path == bookPath) {
      if (title.empty()) title = recent.title;
      if (author.empty()) author = recent.author;
      break;
    }
  }
  if (endsWithIgnoreCase(bookPath, ".epub")) {
    Epub epub(bookPath);
    if (epub.load(true)) {
      if (!epub.getTitle().empty()) title = epub.getTitle();
      if (!epub.getAuthor().empty()) author = epub.getAuthor();
      if (!epub.getLanguage().empty()) language = epub.getLanguage();
    }
  }
  if (title.empty()) {
    const size_t slash = bookPath.find_last_of('/');
    title = (slash == std::string::npos) ? bookPath : bookPath.substr(slash + 1);
  }
  switchTo<EditMetadataActivity>(render, input, bookPath, title, author, language, favorite,
                                 [returnPath]() { onGoToLibrary(returnPath); });
}

/**
 * @brief Set up application.
 */
void reenterDeepSleepWithoutRedraw();

void waitForPowerRelease() {
  while (true) {
    // Use flushInput() here instead of a single debounced update. On wake, especially when short power
    // action is Sleep (10 ms threshold), the debounced currentState can still be "not pressed" even while
    // the physical wake button is down. Flushing samples the raw state and prevents immediate re-sleep.
    gpio.flushInput();
    if (!gpio.isPressed(HalGPIO::BTN_POWER)) {
      break;
    }
    delay(50);
  }
  delay(80);
  gpio.flushInput();
}

void waitForAnyButtonRelease() {
  while (true) {
    gpio.flushInput();
    if (!gpio.isAnyPressed()) {
      break;
    }
    delay(50);
  }
  delay(80);
  gpio.flushInput();
}

PowerWakeAction detectPowerWakeAction(const bool powerWakeCandidate) {
  if (!powerWakeCandidate) {
    return PowerWakeAction::Boot;
  }

  constexpr uint8_t POWER_BUTTON_GPIO = 3;
  pinMode(POWER_BUTTON_GPIO, INPUT_PULLUP);
  const bool wakePressStillHeld = digitalRead(POWER_BUTTON_GPIO) == LOW;
  const unsigned long gestureObservationStartedMs = millis();
  // When the wake-causing key is still down, millis() already includes ROM/Arduino startup. Count that time toward
  // the configured guard; starting here made a physical 1500 ms hold look hundreds of milliseconds too short.
  const unsigned long firstPressStartedMs = wakePressStillHeld ? 0 : gestureObservationStartedMs;
  const uint32_t wakeGuardMs = rtcLastSleepPowerWakeGuardMs;
  const uint32_t firstPressMinMs =
      rtcLastSleepPowerFirstPressMin <= 15 ? static_cast<uint32_t>(rtcLastSleepPowerFirstPressMin) * 100UL : 0;
  const uint32_t secondPressMaxMs = rtcLastSleepPowerSecondPressMax <= 9
                                        ? static_cast<uint32_t>(rtcLastSleepPowerSecondPressMax + 1) * 100UL
                                        : POWER_DOUBLE_PRESS_DEFAULT_SECOND_HOLD_MAX_MS;
  const uint32_t configuredGestureWindowMs =
      rtcLastSleepPowerGestureWindow == 0 || rtcLastSleepPowerGestureWindow > 17
          ? 0
          : 500UL + static_cast<uint32_t>(rtcLastSleepPowerGestureWindow) * 100UL;
  uint32_t secondTapFinishDeadlineMs = configuredGestureWindowMs;
  if (secondTapFinishDeadlineMs == 0) {
    secondTapFinishDeadlineMs = wakeGuardMs > POWER_DOUBLE_PRESS_GUARD_MARGIN_MS
                                    ? wakeGuardMs - POWER_DOUBLE_PRESS_GUARD_MARGIN_MS
                                    : POWER_DOUBLE_PRESS_NO_GUARD_WINDOW_MS;
  }
  if (wakeGuardMs > 0) {
    const uint32_t guardedDeadline =
        wakeGuardMs > POWER_DOUBLE_PRESS_GUARD_MARGIN_MS ? wakeGuardMs - POWER_DOUBLE_PRESS_GUARD_MARGIN_MS : 0;
    secondTapFinishDeadlineMs = std::min(secondTapFinishDeadlineMs, guardedDeadline);
  }
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::PowerGesture, 0,
                                (wakeGuardMs & 0xFFFFUL) | ((secondTapFinishDeadlineMs & 0xFFFFUL) << 16));

  // The X3 power key must be sampled directly before peripheral and input-manager initialization. Once the
  // wake-causing press is released, this boot can never enter the UI; only a valid second tap may change the image.
  while (digitalRead(POWER_BUTTON_GPIO) == LOW) {
    const unsigned long continuouslyHeldMs = millis() - firstPressStartedMs;
    if (continuouslyHeldMs >= POWER_WAKE_EMERGENCY_BOOT_MS || (wakeGuardMs > 0 && continuouslyHeldMs >= wakeGuardMs)) {
      HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::PowerGesture, 4, continuouslyHeldMs);
      return PowerWakeAction::Boot;
    }
    delay(2);
  }

  if (!rtcLastSleepPowerDoublePressEnabled) {
    return PowerWakeAction::ShortPress;
  }
  const unsigned long firstPressFinishedMs = millis();
  const unsigned long firstPressDurationMs = firstPressFinishedMs - firstPressStartedMs;
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::PowerGesture, 1, firstPressDurationMs);
  if (firstPressDurationMs < firstPressMinMs) {
    return PowerWakeAction::ShortPress;
  }

  // Startup time must count toward a continuous hold, but it cannot consume a gesture window before the firmware
  // is capable of observing the second edge. This keeps guard-to-boot timing unchanged while making the configured
  // double-press window physically usable.
  const unsigned long secondTapFinishDeadline = gestureObservationStartedMs + secondTapFinishDeadlineMs;
  while (!timeBefore(secondTapFinishDeadline, millis())) {
    if (digitalRead(POWER_BUTTON_GPIO) != LOW) {
      delay(2);
      continue;
    }

    const unsigned long secondPressStartMs = millis();
    while (digitalRead(POWER_BUTTON_GPIO) == LOW) {
      if (millis() - secondPressStartMs >= POWER_WAKE_EMERGENCY_BOOT_MS) {
        return PowerWakeAction::Boot;
      }
      delay(2);
    }
    const unsigned long secondPressFinishedMs = millis();
    const unsigned long secondPressDurationMs = secondPressFinishedMs - secondPressStartMs;
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::PowerGesture, 2, secondPressDurationMs);
    if (!timeBefore(secondTapFinishDeadline, secondPressFinishedMs) && secondPressDurationMs <= secondPressMaxMs) {
      return PowerWakeAction::DoublePress;
    }
    return PowerWakeAction::ShortPress;
  }

  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::PowerGesture, 3, millis() - gestureObservationStartedMs);
  return PowerWakeAction::ShortPress;
}

extern "C" void initVariant() {
  constexpr uint8_t POWER_BUTTON_GPIO = 3;
  startupTimerWakeCandidate = earlyTimerWakeCandidate();
  if (startupTimerWakeCandidate) {
    rtcExpectedSleepTimerWakeSignature = 0;
    // Timer wakes follow the proven v1.0.20-7 path and must never touch the Power gesture detector.
    startupPowerWakeCandidate = false;
    startupPowerWakeAction = PowerWakeAction::Boot;
    startupPowerWakeDecisionReady = true;
    return;
  }

  pinMode(POWER_BUTTON_GPIO, INPUT_PULLUP);
  const bool powerWasDownAtHook = digitalRead(POWER_BUTTON_GPIO) == LOW;
  rtcExpectedSleepTimerWakeSignature = 0;
  loadEarlyWakeConfig();
  startupPowerWakeCandidate = earlyPowerWakeCandidate() || powerWasDownAtHook;
  startupPowerWakeAction = detectPowerWakeAction(startupPowerWakeCandidate);
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::PowerAction,
                                static_cast<uint32_t>(startupPowerWakeAction),
                                (startupPowerWakeCandidate ? 1UL : 0UL) | (powerWasDownAtHook ? 2UL : 0UL));
  startupPowerWakeDecisionReady = true;
}

bool shouldUsePowerDoublePressForSleepImageAdvance(const PowerWakeAction action) {
  return action == PowerWakeAction::DoublePress &&
         SleepActivity::shouldEnablePowerDoublePressImageAdvance(rtcLastSleepSleptFromReader);
}

bool shouldReSleepForShortPowerWake(const PowerWakeAction action) {
  if (action == PowerWakeAction::Boot) {
    return false;
  }
  if (shouldUsePowerDoublePressForSleepImageAdvance(action)) {
    return false;
  }
  return true;
}

uint8_t wakeReasonCode(const HalGPIO::WakeupReason wakeupReason) {
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      return 1;
    case HalGPIO::WakeupReason::SleepTimer:
      return 2;
    case HalGPIO::WakeupReason::Button:
      return 3;
    case HalGPIO::WakeupReason::AfterFlash:
      return 4;
    case HalGPIO::WakeupReason::AfterUSBPower:
      return 5;
    case HalGPIO::WakeupReason::Other:
    default:
      return 0;
  }
}

void markActivityNow() { lastActivityTime = millis(); }

bool powerSleepGuardActive() { return ignorePowerSleepUntil != 0 && timeBefore(millis(), ignorePowerSleepUntil); }

void normalizeUnavailableClockSettings() {
  if (gpio.deviceIsX3()) {
    return;
  }

  bool changed = false;
  if (SETTINGS.sleepScreen == SystemSetting::DATETIME) {
    SETTINGS.sleepScreen = SystemSetting::LIGHT;
    changed = true;
  }
  if (SETTINGS.sleepClockRefreshInterval != SystemSetting::CLOCK_REFRESH_OFF) {
    SETTINGS.sleepClockRefreshInterval = SystemSetting::CLOCK_REFRESH_OFF;
    changed = true;
  }
  if (changed) {
    SETTINGS.saveToFile();
  }
}

uint32_t sleepImageTimerSecondsFor(const bool sleptFromReader, const bool renderedSleepImage = false) {
  if (!SETTINGS.sleepImageRotationEnabled ||
      (!renderedSleepImage && !SleepActivity::shouldScheduleImageRotation(sleptFromReader))) {
    return 0;
  }
  const uint8_t minutes = SETTINGS.getSleepImageRotationMinutes();
  return minutes == 0 ? 30UL : static_cast<uint32_t>(minutes) * 60UL;
}

uint32_t newsDownloadTimerSeconds() {
  if (!SETTINGS.newsAutoDownload) return 0;
  HalGPIO::DateTime dt;
  if (!gpio.readDateTime(dt) || dt.year < 2024) return 0;
  const int secondsIntoDay = dt.hour * 3600 + dt.minute * 60 + dt.second;
  const int targetSeconds = static_cast<int>(SETTINGS.newsDownloadHour) * 3600;
  int diff = targetSeconds - secondsIntoDay;
  if (diff <= 0) diff += 86400;
  return static_cast<uint32_t>(diff);
}

uint32_t remainingSleepImageTimerSeconds(const uint32_t previousTimerSeconds) {
#ifndef SIMULATOR
  const HalGPIO::SleepTimerTickState timing = HalGPIO::getSleepTimerTickState();
  if (previousTimerSeconds == 0 || timing.durationTicks == 0) {
    return previousTimerSeconds;
  }
  const uint64_t elapsedTicks = timing.currentTicks - timing.startTicks;
  if (elapsedTicks >= timing.durationTicks) {
    return 1;
  }
  const uint64_t remainingTicks = timing.durationTicks - elapsedTicks;
  const uint64_t roundedSeconds =
      (remainingTicks * previousTimerSeconds + timing.durationTicks - 1) / timing.durationTicks;
  return static_cast<uint32_t>(roundedSeconds > 0 ? roundedSeconds : 1);
#else
  return previousTimerSeconds;
#endif
}

void startDeepSleepCycle(const bool sleptFromReader, const bool continueExistingSleep,
                         const bool renderedSleepImage = false, const bool preserveTimerDeadline = false) {
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 30);
  waitForAnyButtonRelease();
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 31);
  const uint32_t configuredTimerSeconds = sleepImageTimerSecondsFor(sleptFromReader, renderedSleepImage);
  const bool previousTimerValid =
      APP_STATE.lastSleepTimerArmSeconds > 0 && APP_STATE.lastSleepTimerArmSeconds <= MAX_SLEEP_IMAGE_TIMER_SECONDS;
  const uint32_t sleepImageTimerSeconds = preserveTimerDeadline && previousTimerValid
                                              ? remainingSleepImageTimerSeconds(APP_STATE.lastSleepTimerArmSeconds)
                                              : configuredTimerSeconds;
  const uint32_t sleepPlanFlags = (sleptFromReader ? 1UL : 0UL) | (continueExistingSleep ? 2UL : 0UL) |
                                  (renderedSleepImage ? 4UL : 0UL) | (SETTINGS.sleepImageRotationEnabled ? 8UL : 0UL) |
                                  (preserveTimerDeadline ? 16UL : 0UL);
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepPlan, sleepImageTimerSeconds, sleepPlanFlags);
  persistEarlyWakeConfig(sleptFromReader);
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 32);
  APP_STATE.lastSleepTimerArmSeconds = sleepImageTimerSeconds;
  if (sleepImageTimerSeconds > 0) {
    APP_STATE.sleepTimerArmCount++;
  }
  APP_STATE.saveToFile();
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 33);
  rtcExpectedSleepTimerWakeSignature = sleepImageTimerSeconds > 0 ? EXPECTED_SLEEP_TIMER_WAKE_SIGNATURE : 0;
  const uint32_t newsTimer = newsDownloadTimerSeconds();
  const uint32_t hardwareTimerSeconds =
      newsTimer > 0 ? (sleepImageTimerSeconds > 0 ? std::min(sleepImageTimerSeconds, newsTimer) : newsTimer)
                    : sleepImageTimerSeconds;
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 34);
  display.deepSleep();
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 35);
#ifndef SIMULATOR
  gpio.prepareDeepSleep(hardwareTimerSeconds, rtcLastSleepPowerDoublePressEnabled);
  if (SETTINGS.persistentSleepLogs) {
    saveSleepWakeTraceCheckpoint();
  }
  gpio.enterPreparedDeepSleep();
#else
  gpio.startDeepSleep(hardwareTimerSeconds);
#endif
}

void reenterDeepSleepWithoutRedraw() {
  startDeepSleepCycle(rtcLastSleepSleptFromReader, /*continueExistingSleep=*/true,
                      /*renderedSleepImage=*/false, /*preserveTimerDeadline=*/true);
}

void enterDeepSleep(const bool forceSleepImageAdvance, const bool continueExistingSleep) {
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 1,
                                (forceSleepImageAdvance ? 1UL : 0UL) | (continueExistingSleep ? 2UL : 0UL) |
                                    (static_cast<uint32_t>(SETTINGS.sleepScreen) << 8));
  normalizeUnavailableClockSettings();
  const bool sleptFromReader = currentActivity != nullptr ? (strcmp(currentActivity->getName(), "Reader") == 0 ||
                                                             strcmp(currentActivity->getName(), "EpubReader") == 0 ||
                                                             strcmp(currentActivity->getName(), "XtcReader") == 0 ||
                                                             strcmp(currentActivity->getName(), "TxtReader") == 0)
                                                          : rtcLastSleepSleptFromReader;
  SleepActivity* sleepActivity = switchTo<SleepActivity>(render, input, sleptFromReader, forceSleepImageAdvance);
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 2);
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::ImageResult, APP_STATE.lastSleepImage,
                                (forceSleepImageAdvance ? 1UL : 0UL) |
                                    (sleepActivity->didRenderSleepImage() ? 2UL : 0UL) |
                                    (continueExistingSleep ? 4UL : 0UL));
  // Persist settings from a clean single-threaded context right before power is cut. switchTo has torn
  // down the previous activity's render task, so this write can't race with concurrent SD reads. This is
  // the durable persist point for in-RAM settings (e.g. dark mode) that a screen toggle may not commit.
  // Back up lastRead to RTC memory so it survives even if the Session file is lost or corrupted.
  const auto& lr = APP_STATE.lastRead;
  const size_t copyLen = std::min(lr.size(), sizeof(rtcLastReadPath) - 1);
  memcpy(rtcLastReadPath, lr.data(), copyLen);
  rtcLastReadPath[copyLen] = '\0';
  Serial.printf("[%lu] [SLEEP] lastRead backed up to RTC: \"%s\"\n", millis(), rtcLastReadPath);
  SETTINGS.saveToFile();
  startDeepSleepCycle(sleptFromReader, continueExistingSleep, sleepActivity->didRenderSleepImage());
}

void setupDisplayAndFonts() {
  display.begin();
  render.begin();
  FontManager::initialize(render);
}

bool handleGlobalPowerRefresh() {
  if (!currentActivity || !currentActivity->allowGlobalPowerRefresh()) {
    return false;
  }
  if (SETTINGS.shortPwrBtn != SystemSetting::SHORT_PWRBTN::PAGE_REFRESH) {
    return false;
  }
  if (!input.wasReleased(MappedInputManager::Button::Power)) {
    return false;
  }

  renderer.displayBuffer(HalDisplay::MANUAL_REFRESH);
  return true;
}

/**
 * @brief Set up application.
 */
void setup() {
  t1 = millis();
  constexpr uint8_t POWER_BUTTON_GPIO = 3;
  if (!startupPowerWakeDecisionReady) {
    startupTimerWakeCandidate = earlyTimerWakeCandidate();
    rtcExpectedSleepTimerWakeSignature = 0;
    if (!startupTimerWakeCandidate) {
      pinMode(POWER_BUTTON_GPIO, INPUT_PULLUP);
      const bool powerWasDownAtSetup = digitalRead(POWER_BUTTON_GPIO) == LOW;
      loadEarlyWakeConfig();
      startupPowerWakeCandidate = earlyPowerWakeCandidate() || powerWasDownAtSetup;
      startupPowerWakeAction = detectPowerWakeAction(startupPowerWakeCandidate);
      HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::PowerAction,
                                    static_cast<uint32_t>(startupPowerWakeAction),
                                    (startupPowerWakeCandidate ? 1UL : 0UL) | (powerWasDownAtSetup ? 2UL : 0UL));
    }
  }
  const bool powerWakeCandidate = startupPowerWakeCandidate;
  const PowerWakeAction powerWakeAction = startupPowerWakeAction;
  gpio.begin();
  HalGPIO::WakeupReason wakeupReason = gpio.getWakeupReason();
  if (startupTimerWakeCandidate) {
    wakeupReason = HalGPIO::WakeupReason::SleepTimer;
  } else if (powerWakeCandidate && wakeupReason != HalGPIO::WakeupReason::AfterFlash) {
    wakeupReason = HalGPIO::WakeupReason::PowerButton;
  }
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SetupWake, wakeReasonCode(wakeupReason),
                                static_cast<uint32_t>(powerWakeAction));
  Serial.printf("[%lu] [BOOT] Display init\n", millis());
  setupDisplayAndFonts();
  EInkDisplay::setWaitCallback([] {
    gpio.update();
    if (gpio.isPressed(HalGPIO::BTN_POWER)) {
      Serial.printf("[%lu] [DBG] WAIT-CB power pressed, held=%lums\n", millis(), gpio.getHeldTime());
    }
  });
  Serial.printf("[%lu] [BOOT] Display OK, heap=%lu\n", millis(), (unsigned long)esp_get_free_heap_size());
  confirmBootedOtaImage();

  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 3000) delay(10);
  }

  Serial.printf("[%lu] [BOOT] SD init\n", millis());
  if (!SdMan.begin()) {
    switchTo<FullScreenMessageActivity>(render, input, "SD card error", EpdFontFamily::BOLD);
    return;
  }
  Serial.printf("[%lu] [BOOT] SD OK\n", millis());
  APP_STATE.loadFromFile();
  Serial.printf("[%lu] [BOOT] APP_STATE.lastRead = \"%s\", rtcLastReadPath = \"%s\"\n", millis(),
                APP_STATE.lastRead.c_str(), rtcLastReadPath);

  Serial.printf("[%lu] [BOOT] Loading settings\n", millis());
  SETTINGS.loadFromFile();
  Serial.printf("[%lu] [BOOT] Settings OK, darkMode=%u, heap=%lu\n", millis(), SETTINGS.darkMode,
                (unsigned long)esp_get_free_heap_size());
  OPDS_STORE.loadOrMigrate({"Default", SETTINGS.opdsServerUrl, SETTINGS.opdsUsername, SETTINGS.opdsPassword});
#ifndef SIMULATOR
  // A normal wake retains the preceding SleepPlan in RTC memory. Persist it only when restart-safe logs are enabled.
  if (SETTINGS.persistentSleepLogs) {
    saveSleepWakeTraceCheckpoint(/*requireRetainedSleepCycle=*/true);
  }
#endif
  normalizeUnavailableClockSettings();
  APP_STATE.lastWakeReason = wakeReasonCode(wakeupReason);
  if (wakeupReason == HalGPIO::WakeupReason::SleepTimer) {
    APP_STATE.sleepTimerWakeCount++;
  }
  APP_STATE.saveToFile();
  render.setDarkMode(SETTINGS.darkMode);
  render.setFadingFix(SETTINGS.sunlightFadingFix);
  // Booting/waking into dark mode flips the whole panel's polarity; force the first paint to be a full
  // refresh so it develops cleanly instead of ghosting from the previous (light) sleep image.
  if (SETTINGS.darkMode) {
    render.requestNextFullRefresh();
  }

  gpio.flushInput();

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      if (shouldUsePowerDoublePressForSleepImageAdvance(powerWakeAction)) {
        HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::DoublePressBranch);
        enterDeepSleep(/*forceSleepImageAdvance=*/true, /*continueExistingSleep=*/true);
        return;
      }
      if (shouldReSleepForShortPowerWake(powerWakeAction)) {
        HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::ShortPressBranch);
        reenterDeepSleepWithoutRedraw();
        return;
      }
      break;
    case HalGPIO::WakeupReason::SleepTimer:
      HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::TimerBranch);
      if (SETTINGS.newsAutoDownload) {
        NewsActivity::tryAutoDownload();
      }
      enterDeepSleep(/*forceSleepImageAdvance=*/true, /*continueExistingSleep=*/true);
      return;
    case HalGPIO::WakeupReason::AfterUSBPower:
      reenterDeepSleepWithoutRedraw();
      return;
    default:
      break;
  }

  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::UiBranch, wakeReasonCode(wakeupReason),
                                static_cast<uint32_t>(powerWakeAction));
  switchTo<BootActivity>(render, input);
  waitForPowerRelease();
  markActivityNow();
  ignorePowerSleepUntil = millis() + WAKE_POWER_GUARD_MS;
}

/**
 * @brief All activity loop.
 */
void loop() {
  gpio.update();

  if (lastActivityTime == 0) {
    markActivityNow();
  }

  if (powerSleepGuardActive()) {
    markActivityNow();
  }

  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || (currentActivity && currentActivity->preventAutoSleep())) {
    markActivityNow();
  }

  if (millis() - lastActivityTime >= SETTINGS.getSleepTimeoutMs()) {
    enterDeepSleep();
    return;
  }

  if (!powerSleepGuardActive() && gpio.isPressed(HalGPIO::BTN_POWER)) {
    const unsigned long heldMs = gpio.getHeldTime();
    Serial.printf("[%lu] [DBG] PRE-LOOP power held %lums (need %lums)\n", millis(), heldMs,
                  (unsigned long)SETTINGS.getPowerButtonDuration());
    if (heldMs >= 10000) {
      esp_restart();
      return;
    }
    if (heldMs > SETTINGS.getPowerButtonDuration()) {
      Serial.printf("[%lu] [DBG] PRE-LOOP -> enterDeepSleep\n", millis());
      enterDeepSleep();
      return;
    }
  }

  if (handleGlobalPowerRefresh()) {
    delay(10);
    return;
  }

  // Keep the renderer's dark-mode flag in sync with settings so every screen paints consistently.
  render.setDarkMode(SETTINGS.darkMode);

  if (currentActivity) {
    currentActivity->loop();
  }

  if (!powerSleepGuardActive() && gpio.isPressed(HalGPIO::BTN_POWER)) {
    const unsigned long heldMs = gpio.getHeldTime();
    Serial.printf("[%lu] [DBG] POST-LOOP power held %lums\n", millis(), heldMs);
    if (heldMs >= 10000) {
      esp_restart();
      return;
    }
    if (heldMs > SETTINGS.getPowerButtonDuration()) {
      Serial.printf("[%lu] [DBG] POST-LOOP -> enterDeepSleep\n", millis());
      enterDeepSleep();
      return;
    }
  }

  if (currentActivity && currentActivity->skipLoopDelay()) {
    yield();
  } else {
    delay(10);
  }
}
