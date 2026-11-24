/**
 * @file SystemSetting.cpp
 * @brief Definitions for SystemSetting.
 */

#include "state/SystemSetting.h"

#ifndef INX_SIMULATOR_WEB_ONLY
#include <GfxRenderer.h>
#include <HalDisplay.h>
#endif
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "state/ReaderSetting.h"

#ifndef INX_SIMULATOR_WEB_ONLY
#include "system/FontManager.h"
#include "system/Fonts.h"
#endif

SystemSetting SystemSetting::instance;

/**
 * @brief Reads a value from file and validates it's within allowed range
 * @param file File to read from
 * @param member Reference to member to update
 * @param maxValue Maximum allowed value
 */
void readAndValidate(FsFile& file, uint8_t& member, const uint8_t maxValue) {
  uint8_t tempValue;
  serialization::readPod(file, tempValue);

  if (tempValue < maxValue) {
    member = tempValue;
  }
}

namespace {
// E-inx: 44 is upstream's 37 layout (below, unchanged) with the E-inx fields appended after it.
constexpr uint8_t SETTINGS_FILE_VERSION = 44;
// E-inx files up to 43 are the pre-split layout, and loadLegacyEinxSettings() migrates them.
constexpr uint8_t LEGACY_SETTINGS_FILE_VERSION = 43;
constexpr uint8_t LEGACY_SETTINGS_COUNT = 102;
/** Last field index in the v9 layout (1-based count of persisted pods through displayImageDither). */
constexpr uint8_t LEGACY_SETTINGS_COUNT_V9 = 40;
constexpr uint8_t LEGACY_SLEEP_IMAGE_ADVANCE_POWER = 7;
// Reader-related fields (font/layout, status bar, refresh frequency, page auto-turn, image
// grayscale, per-button reader actions, XTC reader settings, hyphenation, bionic reading, screen
// margin, orientation, dictionary folder...) moved out to ReaderSetting/reader_settings.bin as of
// this version. Files older than this used an incompatible field layout and can't be positionally
// parsed, so loadFromFile() discards them and starts fresh instead of attempting to read them. See
// ReaderSetting.cpp for the new reader_settings.bin format (independently versioned from 1).
constexpr uint8_t MIN_SUPPORTED_SETTINGS_VERSION = 44;
// Must equal the number of data fields read by the do-while loop in loadFromFile() (currently 74:
// upstream's 40 through showMenuClock, then 33 E-inx fields through deviceName - NOT counting the version/count header
// fields read separately before the loop). This is written into the file as its own "how many fields do I contain"
// header and read back as fileSettingsCount; the loop's `settingsRead < fileSettingsCount` checks use it to know
// whether the tail fields are actually present. If it's wrong, either the tail fields never get read back even though
// they were written (undercount), or the file never triggers the self-healing rewrite on old-format files (overcount,
// since fileSettingsCount can never reach it).
constexpr uint8_t SETTINGS_COUNT = 73;
constexpr uint8_t LEGACY_IMAGE_PRESENTATION_COUNT = 4;
constexpr char SETTINGS_FILE[] = "/.system/settings.bin";
constexpr char UI_THEME_FILE[] = "/.system/ui_theme.bin";
constexpr uint32_t FNV1A_OFFSET = 2166136261UL;
constexpr uint32_t FNV1A_PRIME = 16777619UL;

void hashBytes(uint32_t& hash, const void* data, const size_t len) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    hash ^= bytes[i];
    hash *= FNV1A_PRIME;
  }
}

template <typename T>
void hashPod(uint32_t& hash, const T& value) {
  hashBytes(hash, &value, sizeof(T));
}

void hashString(uint32_t& hash, const char* value) {
  const uint32_t len = value == nullptr ? 0 : static_cast<uint32_t>(strlen(value));
  hashPod(hash, len);
  if (len > 0) {
    hashBytes(hash, value, len);
  }
}

bool hashFile(const char* path, uint32_t& hash) {
  FsFile file;
  if (!SdMan.openFileForRead("CPS", path, file)) {
    return false;
  }
  hash = FNV1A_OFFSET;
  uint8_t buffer[64];
  while (file.available()) {
    const int n = file.read(buffer, sizeof(buffer));
    if (n <= 0) {
      break;
    }
    hashBytes(hash, buffer, static_cast<size_t>(n));
  }
  file.close();
  return true;
}

void sanitizeSleepCustomBmp(char* buf) {
  if (buf == nullptr || buf[0] == '\0') {
    return;
  }
  if (strcmp(buf, "/sleep.bmp") == 0 || strcmp(buf, "/sleep.jpg") == 0 || strcmp(buf, "/sleep.jpeg") == 0) {
    return;
  }
  if (strstr(buf, "..") != nullptr) {
    buf[0] = '\0';
    return;
  }
  for (const char* p = buf; *p != '\0'; ++p) {
    if (*p == '/' || *p == '\\' || *p == ':') {
      buf[0] = '\0';
      return;
    }
  }
}

bool validRefreshFrequency(const uint8_t value) {
  return value == 1 || value == 5 || value == 10 || value == 15 || value == 30;
}

void saveUiThemeSetting(const uint8_t value) {
  FsFile file;
  if (!SdMan.openFileForWrite("CPS", UI_THEME_FILE, file)) {
    return;
  }
  serialization::writePod(file, value);
  file.close();
}

bool loadUiThemeSetting(uint8_t& value) {
  FsFile file;
  if (!SdMan.openFileForRead("CPS", UI_THEME_FILE, file)) {
    return false;
  }

  uint8_t saved = SystemSetting::UI_THEME_CLASSIC;
  serialization::readPod(file, saved);
  file.close();

  if (saved >= SystemSetting::UI_THEME_COUNT) {
    return false;
  }
  value = saved;
  return true;
}

uint8_t normalizeSleepImageRotationMinutes(uint8_t value) {
  constexpr uint8_t kMin = 5;
  constexpr uint8_t kMax = 120;
  if (value == 0) {
    return 0;
  }
  if (value < kMin) {
    return kMin;
  }
  if (value > kMax) {
    value = kMax;
  }
  value = static_cast<uint8_t>(((value + 2) / 5) * 5);
  if (value < kMin) {
    return kMin;
  }
  if (value > kMax) {
    return kMax;
  }
  return value;
}

uint32_t settingsHash(const SystemSetting& settings) {
  uint32_t hash = FNV1A_OFFSET;
  hashPod(hash, SETTINGS_FILE_VERSION);
  hashPod(hash, SETTINGS_COUNT);
  hashPod(hash, settings.sleepScreen);
  hashPod(hash, settings.shortPwrBtn);
  hashPod(hash, settings.frontButtonLayout);
  hashPod(hash, settings.sideButtonLayout);
  hashPod(hash, settings.sleepTimeout);
  hashPod(hash, settings.sleepScreenCoverMode);
  hashString(hash, settings.opdsServerUrl);
  hashPod(hash, settings.hideBatteryPercentage);
  hashString(hash, settings.opdsUsername);
  hashString(hash, settings.opdsPassword);
  hashPod(hash, settings.sleepScreenCoverFilter);
  hashPod(hash, settings.useLibraryIndex);
  hashPod(hash, settings.recentLibraryMode);
  hashPod(hash, settings.bootSetting);
  hashPod(hash, settings.sleepImageQuality);
  hashString(hash, settings.sleepCustomBmp);
  hashPod(hash, settings.displayImageDither);
  hashPod(hash, settings.legacyDisplayImagePresentation);
  hashPod(hash, settings.refreshOnLoadRecent);
  hashPod(hash, settings.refreshOnLoadLibrary);
  hashPod(hash, settings.refreshOnLoadSettings);
  hashPod(hash, settings.refreshOnLoadSync);
  hashPod(hash, settings.refreshOnLoadStatistics);
  hashPod(hash, settings.bitmapRoundedCorners);
  hashPod(hash, settings.recentVisibleCount);
  hashPod(hash, settings.librarySortEnabled);
  hashPod(hash, settings.librarySortMode);
  hashPod(hash, settings.libraryMode);
  hashPod(hash, settings.libraryViewMode);
  hashPod(hash, settings.sleepClockStyle);
  hashPod(hash, settings.sleepClockTimeFormat);
  hashPod(hash, settings.timeZoneQuarterOffset);
  hashPod(hash, settings.mainMenuNav);
  hashPod(hash, settings.sleepClockRefreshInterval);
  hashPod(hash, settings.shakePageTurn);
  hashPod(hash, settings.shakePageTurnSensitivity);
  hashPod(hash, settings.uiTheme);
  hashPod(hash, settings.libraryShelfEnabled);
  hashPod(hash, settings.hideButtonHints);
  hashPod(hash, settings.showMenuClock);
  // E-inx fields
  hashPod(hash, settings.darkMode);
  hashPod(hash, settings.dailyReadingGoal);
  hashPod(hash, settings.readerRefreshMode);
  hashPod(hash, settings.sunlightFadingFix);
  hashPod(hash, settings.antiGhostingExperimental);
  hashPod(hash, settings.sleepImageRotationMinutes);
  hashPod(hash, settings.sleepImageRotationEnabled);
  hashPod(hash, settings.sleepImagePowerDoublePress);
  hashPod(hash, settings.powerWakeGuard);
  hashPod(hash, settings.sleepImagePowerGestureWindow);
  hashPod(hash, settings.statusBarInnerLeft);
  hashPod(hash, settings.statusBarInnerRight);
  hashPod(hash, settings.sleepImagePowerFirstPressMin);
  hashPod(hash, settings.sleepImagePowerSecondPressMax);
  hashPod(hash, settings.persistentSleepLogs);
  hashString(hash, settings.newsRepoUrl);
  hashPod(hash, settings.newsAutoDownload);
  hashPod(hash, settings.newsDownloadHour);
  hashPod(hash, settings.x3ReinforceReader);
  hashPod(hash, settings.x3ReinforceUi);
  hashPod(hash, settings.x3ReinforceThumbnails);
  hashPod(hash, settings.x3ReinforcePeriodicClean);
  hashPod(hash, settings.x3ReinforceCleanInterval);
  hashPod(hash, settings.tab2Content);
  hashPod(hash, settings.appDrawerNews);
  hashPod(hash, settings.appDrawerGames);
  hashPod(hash, settings.appDrawerCrossPlay);
  hashPod(hash, settings.appDrawerAgentIsland);
  hashString(hash, settings.otaReleaseUrl);
  hashPod(hash, settings.x3PageWaveform);
  hashPod(hash, settings.x3MaintenanceAction);
  hashPod(hash, settings.x3MaintenancePasses);
  hashString(hash, settings.deviceName);
  return hash;
}

}  // namespace

const char* SystemSetting::readerButtonActionLabel(const uint8_t action) {
  static const char* const kLabels[] = {"None",          "Page Next",         "Page Previous",
                                        "Open Settings", "Annotate",          "Dictionary",
                                        "Page Refresh",  "Chapter Skip Next", "Chapter Skip Previous",
                                        "Bookmark",      "Table of Contents", "Change Orientation",
                                        "Apply Preset",  "Quick Actions"};
  if (action >= SystemSetting::READER_BUTTON_ACTION_COUNT) {
    return "None";
  }
  return kLabels[action];
}

void SystemSetting::setSleepCustomBmpFromInput(const char* s) {
  if (s == nullptr || s[0] == '\0') {
    sleepCustomBmp[0] = '\0';
    return;
  }
  strncpy(sleepCustomBmp, s, sizeof(sleepCustomBmp) - 1);
  sleepCustomBmp[sizeof(sleepCustomBmp) - 1] = '\0';
  sanitizeSleepCustomBmp(sleepCustomBmp);
}

/**
 * @brief Saves all settings to file
 * @return true if save successful, false otherwise
 */
bool SystemSetting::saveToFile() const {
  {
    SystemSetting* mut = const_cast<SystemSetting*>(this);
    if (mut->recentVisibleCount < 1 || mut->recentVisibleCount > 9) mut->recentVisibleCount = 9;
    if (mut->librarySortEnabled > 1) mut->librarySortEnabled = 1;
    if (mut->libraryShelfEnabled > 1) mut->libraryShelfEnabled = 0;
    if (mut->librarySortMode > 7) mut->librarySortMode = 0;
    if (mut->libraryMode >= LIBRARY_MODE_COUNT) mut->libraryMode = LIBRARY_GRID;
    if (mut->libraryViewMode >= LIBRARY_VIEW_MODE_COUNT ||
        (mut->libraryViewMode == LIBRARY_VIEW_SHELF && mut->libraryShelfEnabled == 0))
      mut->libraryViewMode = LIBRARY_VIEW_FOLDERS;
    if (mut->sleepClockStyle >= SLEEP_CLOCK_STYLE_COUNT) mut->sleepClockStyle = CLOCK_CENTERED_DATE;
    if (mut->sleepClockTimeFormat >= CLOCK_TIME_FORMAT_COUNT) mut->sleepClockTimeFormat = CLOCK_24_HOUR;
    if (mut->sleepClockRefreshInterval >= CLOCK_REFRESH_INTERVAL_COUNT)
      mut->sleepClockRefreshInterval = CLOCK_REFRESH_OFF;
    if (mut->sleepImageQuality >= SLEEP_IMAGE_QUALITY_COUNT) mut->sleepImageQuality = SLEEP_IMAGE_HIGH;
    if (mut->timeZoneQuarterOffset > 104) mut->timeZoneQuarterOffset = 80;
    if (mut->shakePageTurn > 2) mut->shakePageTurn = 0;
    if (mut->shakePageTurnSensitivity > 2) mut->shakePageTurnSensitivity = 1;
    if (mut->uiTheme >= UI_THEME_COUNT) mut->uiTheme = UI_THEME_CLASSIC;
    if (mut->hideButtonHints > 1) mut->hideButtonHints = 0;
    if (mut->showMenuClock > 1) mut->showMenuClock = 1;
    mut->sanitizeEinxFields();
  }

  const uint32_t currentHash = settingsHash(*this);
  uint32_t storedHash = 0;
  if (hashFile(SETTINGS_FILE, storedHash) && storedHash == currentHash) {
    return true;
  }

  SdMan.mkdir("/.system");

  FsFile outputFile;

  if (!SdMan.openFileForWrite("CPS", SETTINGS_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, SETTINGS_FILE_VERSION);
  serialization::writePod(outputFile, SETTINGS_COUNT);
  serialization::writePod(outputFile, sleepScreen);
  serialization::writePod(outputFile, shortPwrBtn);
  serialization::writePod(outputFile, frontButtonLayout);
  serialization::writePod(outputFile, sideButtonLayout);
  serialization::writePod(outputFile, sleepTimeout);
  serialization::writePod(outputFile, sleepScreenCoverMode);
  serialization::writeString(outputFile, std::string(opdsServerUrl));
  serialization::writePod(outputFile, hideBatteryPercentage);
  serialization::writeString(outputFile, std::string(opdsUsername));
  serialization::writeString(outputFile, std::string(opdsPassword));
  serialization::writePod(outputFile, sleepScreenCoverFilter);
  serialization::writePod(outputFile, useLibraryIndex);
  serialization::writePod(outputFile, recentLibraryMode);
  serialization::writePod(outputFile, bootSetting);
  serialization::writePod(outputFile, sleepImageQuality);
  serialization::writeString(outputFile, std::string(sleepCustomBmp));
  serialization::writePod(outputFile, displayImageDither);
  serialization::writePod(outputFile, legacyDisplayImagePresentation);
  serialization::writePod(outputFile, refreshOnLoadRecent);
  serialization::writePod(outputFile, refreshOnLoadLibrary);
  serialization::writePod(outputFile, refreshOnLoadSettings);
  serialization::writePod(outputFile, refreshOnLoadSync);
  serialization::writePod(outputFile, refreshOnLoadStatistics);
  serialization::writePod(outputFile, bitmapRoundedCorners);
  serialization::writePod(outputFile, recentVisibleCount);
  serialization::writePod(outputFile, librarySortEnabled);
  serialization::writePod(outputFile, librarySortMode);
  serialization::writePod(outputFile, libraryMode);
  serialization::writePod(outputFile, libraryViewMode);
  serialization::writePod(outputFile, sleepClockStyle);
  serialization::writePod(outputFile, sleepClockTimeFormat);
  serialization::writePod(outputFile, timeZoneQuarterOffset);
  serialization::writePod(outputFile, mainMenuNav);
  serialization::writePod(outputFile, sleepClockRefreshInterval);
  serialization::writePod(outputFile, shakePageTurn);
  serialization::writePod(outputFile, shakePageTurnSensitivity);
  serialization::writePod(outputFile, uiTheme);
  serialization::writePod(outputFile, libraryShelfEnabled);
  serialization::writePod(outputFile, hideButtonHints);
  serialization::writePod(outputFile, showMenuClock);
  // E-inx fields, after upstream's: keep this order in step with settingsHash() and loadFromFile().
  serialization::writePod(outputFile, darkMode);
  serialization::writePod(outputFile, dailyReadingGoal);
  serialization::writePod(outputFile, readerRefreshMode);
  serialization::writePod(outputFile, sunlightFadingFix);
  serialization::writePod(outputFile, antiGhostingExperimental);
  serialization::writePod(outputFile, sleepImageRotationMinutes);
  serialization::writePod(outputFile, sleepImageRotationEnabled);
  serialization::writePod(outputFile, sleepImagePowerDoublePress);
  serialization::writePod(outputFile, powerWakeGuard);
  serialization::writePod(outputFile, sleepImagePowerGestureWindow);
  serialization::writePod(outputFile, statusBarInnerLeft);
  serialization::writePod(outputFile, statusBarInnerRight);
  serialization::writePod(outputFile, sleepImagePowerFirstPressMin);
  serialization::writePod(outputFile, sleepImagePowerSecondPressMax);
  serialization::writePod(outputFile, persistentSleepLogs);
  serialization::writeString(outputFile, std::string(newsRepoUrl));
  serialization::writePod(outputFile, newsAutoDownload);
  serialization::writePod(outputFile, newsDownloadHour);
  serialization::writePod(outputFile, x3ReinforceReader);
  serialization::writePod(outputFile, x3ReinforceUi);
  serialization::writePod(outputFile, x3ReinforceThumbnails);
  serialization::writePod(outputFile, x3ReinforcePeriodicClean);
  serialization::writePod(outputFile, x3ReinforceCleanInterval);
  serialization::writePod(outputFile, tab2Content);
  serialization::writePod(outputFile, appDrawerNews);
  serialization::writePod(outputFile, appDrawerGames);
  serialization::writePod(outputFile, appDrawerCrossPlay);
  serialization::writePod(outputFile, appDrawerAgentIsland);
  serialization::writeString(outputFile, std::string(otaReleaseUrl));
  serialization::writePod(outputFile, x3PageWaveform);
  serialization::writePod(outputFile, x3MaintenanceAction);
  serialization::writePod(outputFile, x3MaintenancePasses);
  serialization::writeString(outputFile, std::string(deviceName));

  outputFile.close();
  saveUiThemeSetting(uiTheme);

  Serial.printf("[%lu] [CPS] Settings saved to file (version %u, darkMode=%u)\n", millis(), SETTINGS_FILE_VERSION,
                darkMode);
  return true;
}

/**
 * @brief Loads all settings from file
 * @return true if load successful, false otherwise
 */
bool SystemSetting::loadFromFile() {
  FsFile inputFile;

  if (!SdMan.openFileForRead("CPS", SETTINGS_FILE, inputFile)) {
    saveToFile();
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);

  if (version <= LEGACY_SETTINGS_FILE_VERSION) {
    inputFile.close();
    return loadLegacyEinxSettings();
  }

  if (version > SETTINGS_FILE_VERSION || version < MIN_SUPPORTED_SETTINGS_VERSION) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unsupported version %u (expected %u-%u)\n", millis(), version,
                  MIN_SUPPORTED_SETTINGS_VERSION, SETTINGS_FILE_VERSION);
    inputFile.close();
    return false;
  }

  const bool shouldRewriteSettings = version < SETTINGS_FILE_VERSION || fileSettingsCount < SETTINGS_COUNT;
  uint8_t settingsRead = 0;

  do {
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepTimeout, SLEEP_TIMEOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;

    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, useLibraryIndex);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, recentLibraryMode, RECENT_LIBRARY_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, bootSetting, BOOT_SETTING_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, sleepImageQuality);
    if (sleepImageQuality >= SLEEP_IMAGE_QUALITY_COUNT) sleepImageQuality = SLEEP_IMAGE_HIGH;
    if (++settingsRead >= fileSettingsCount) break;

    {
      std::string sleepBmpStr;
      serialization::readString(inputFile, sleepBmpStr);
      setSleepCustomBmpFromInput(sleepBmpStr.c_str());
    }
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, displayImageDither, READER_IMAGE_DITHER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, legacyDisplayImagePresentation, LEGACY_IMAGE_PRESENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, refreshOnLoadRecent);
    if (refreshOnLoadRecent > 1) refreshOnLoadRecent = 0;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, refreshOnLoadLibrary);
    if (refreshOnLoadLibrary > 1) refreshOnLoadLibrary = 0;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, refreshOnLoadSettings);
    if (refreshOnLoadSettings > 1) refreshOnLoadSettings = 0;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, refreshOnLoadSync);
    if (refreshOnLoadSync > 1) refreshOnLoadSync = 0;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, refreshOnLoadStatistics);
    if (refreshOnLoadStatistics > 1) refreshOnLoadStatistics = 0;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, bitmapRoundedCorners);
    if (bitmapRoundedCorners > 2) bitmapRoundedCorners = 0;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, recentVisibleCount);
    if (recentVisibleCount < 1 || recentVisibleCount > 9) recentVisibleCount = 9;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, librarySortEnabled);
    if (librarySortEnabled > 1) librarySortEnabled = 1;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, librarySortMode);
    if (librarySortMode > 7) librarySortMode = 0;
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, libraryMode, LIBRARY_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, libraryViewMode, LIBRARY_VIEW_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepClockStyle, SLEEP_CLOCK_STYLE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepClockTimeFormat, CLOCK_TIME_FORMAT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, timeZoneQuarterOffset);
    if (timeZoneQuarterOffset > 104) timeZoneQuarterOffset = 80;
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, mainMenuNav, MAIN_MENU_NAV_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepClockRefreshInterval, CLOCK_REFRESH_INTERVAL_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, shakePageTurn);
    if (shakePageTurn > 2) shakePageTurn = 0;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, shakePageTurnSensitivity);
    if (shakePageTurnSensitivity > 2) shakePageTurnSensitivity = 1;
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, uiTheme, UI_THEME_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, libraryShelfEnabled);
    if (libraryShelfEnabled > 1) libraryShelfEnabled = 0;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, hideButtonHints);
    if (hideButtonHints > 1) hideButtonHints = 0;
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, showMenuClock);
    if (showMenuClock > 1) showMenuClock = 1;
    if (++settingsRead >= fileSettingsCount) break;

    // E-inx fields; out-of-range values are fixed by sanitizeEinxFields() below.
    serialization::readPod(inputFile, darkMode);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, dailyReadingGoal);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, readerRefreshMode);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sunlightFadingFix);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, antiGhostingExperimental);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sleepImageRotationMinutes);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sleepImageRotationEnabled);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sleepImagePowerDoublePress);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, powerWakeGuard);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sleepImagePowerGestureWindow);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarInnerLeft);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, statusBarInnerRight);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sleepImagePowerFirstPressMin);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, sleepImagePowerSecondPressMax);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, persistentSleepLogs);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string value;
      serialization::readString(inputFile, value);
      strncpy(newsRepoUrl, value.c_str(), sizeof(newsRepoUrl) - 1);
      newsRepoUrl[sizeof(newsRepoUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, newsAutoDownload);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, newsDownloadHour);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, x3ReinforceReader);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, x3ReinforceUi);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, x3ReinforceThumbnails);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, x3ReinforcePeriodicClean);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, x3ReinforceCleanInterval);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, tab2Content);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, appDrawerNews);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, appDrawerGames);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, appDrawerCrossPlay);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, appDrawerAgentIsland);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string value;
      serialization::readString(inputFile, value);
      strncpy(otaReleaseUrl, value.c_str(), sizeof(otaReleaseUrl) - 1);
      otaReleaseUrl[sizeof(otaReleaseUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, x3PageWaveform);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, x3MaintenanceAction);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, x3MaintenancePasses);
    if (++settingsRead >= fileSettingsCount) break;
    {
      std::string value;
      serialization::readString(inputFile, value);
      strncpy(deviceName, value.c_str(), sizeof(deviceName) - 1);
      deviceName[sizeof(deviceName) - 1] = '\0';
    }
    ++settingsRead;

  } while (false);

  inputFile.close();

  if (recentVisibleCount < 1 || recentVisibleCount > 9) recentVisibleCount = 9;
  if (librarySortEnabled > 1) librarySortEnabled = 1;
  if (libraryShelfEnabled > 1) libraryShelfEnabled = 0;
  if (hideButtonHints > 1) hideButtonHints = 0;
  if (showMenuClock > 1) showMenuClock = 1;
  if (librarySortMode > 7) librarySortMode = 0;
  if (sleepClockStyle >= SLEEP_CLOCK_STYLE_COUNT) sleepClockStyle = CLOCK_CENTERED_DATE;
  if (sleepClockTimeFormat >= CLOCK_TIME_FORMAT_COUNT) sleepClockTimeFormat = CLOCK_24_HOUR;
  if (sleepClockRefreshInterval >= CLOCK_REFRESH_INTERVAL_COUNT) sleepClockRefreshInterval = CLOCK_REFRESH_OFF;
  if (sleepImageQuality >= SLEEP_IMAGE_QUALITY_COUNT) sleepImageQuality = SLEEP_IMAGE_HIGH;
  if (timeZoneQuarterOffset > 104) timeZoneQuarterOffset = 80;
  if (libraryMode >= LIBRARY_MODE_COUNT) libraryMode = LIBRARY_GRID;
  if (libraryViewMode >= LIBRARY_VIEW_MODE_COUNT ||
      (libraryViewMode == LIBRARY_VIEW_SHELF && libraryShelfEnabled == 0)) {
    libraryViewMode = LIBRARY_VIEW_FOLDERS;
  }
  if (uiTheme >= UI_THEME_COUNT) uiTheme = UI_THEME_CLASSIC;
  loadUiThemeSetting(uiTheme);
  sanitizeEinxFields();

  Serial.printf("[%lu] [CPS] Settings loaded (version %u, %u items, darkMode=%u)\n", millis(), version, settingsRead,
                darkMode);

  if (shouldRewriteSettings) {
    saveToFile();
  }

  return true;
}

/**
 * @brief Gets sleep timeout in milliseconds
 * @return Sleep timeout in milliseconds
 */
unsigned long SystemSetting::getSleepTimeoutMs() const {
  switch (sleepTimeout) {
    case SLEEP_1_MIN:
      return 1UL * 60 * 1000;
    case SLEEP_5_MIN:
      return 5UL * 60 * 1000;
    case SLEEP_10_MIN:
    default:
      return 10UL * 60 * 1000;
    case SLEEP_15_MIN:
      return 15UL * 60 * 1000;
    case SLEEP_30_MIN:
      return 30UL * 60 * 1000;
  }
}

int SystemSetting::getTimeZoneOffsetMinutes() const {
  const int quarterHours = static_cast<int>(timeZoneQuarterOffset) - 48;
  return quarterHours * 15;
}

void SystemSetting::formatTimeZone(char* out, size_t outSize) const {
  if (out == nullptr || outSize == 0) {
    return;
  }
  const int minutes = getTimeZoneOffsetMinutes();
  const char sign = minutes < 0 ? '-' : '+';
  const int absMinutes = minutes < 0 ? -minutes : minutes;
  std::snprintf(out, outSize, "UTC%c%02d:%02d", sign, absMinutes / 60, absMinutes % 60);
}

void SystemSetting::runHalfRefreshOnLoadIfEnabled(const GfxRenderer& renderer, const RefreshOnLoadPage page) const {
#ifdef INX_SIMULATOR_WEB_ONLY
  (void)renderer;
  (void)page;
#else
  uint8_t on = 0;
  switch (page) {
    case RefreshOnLoadPage::Recent:
      on = refreshOnLoadRecent;
      break;
    case RefreshOnLoadPage::Library:
      on = refreshOnLoadLibrary;
      break;
    case RefreshOnLoadPage::Settings:
      on = refreshOnLoadSettings;
      break;
    case RefreshOnLoadPage::Sync:
      on = refreshOnLoadSync;
      break;
    case RefreshOnLoadPage::Statistics:
      on = refreshOnLoadStatistics;
      break;
  }
  if (on) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
#endif
}

/** Clamps the E-inx-only fields to their valid ranges; shared by load, save and the legacy migration. */
void SystemSetting::sanitizeEinxFields() {
  if (dailyReadingGoal >= DAILY_READING_GOAL_COUNT) dailyReadingGoal = DAILY_GOAL_30_MIN;
  if (readerRefreshMode >= READER_REFRESH_MODE_COUNT) readerRefreshMode = READER_REFRESH_AUTO;
  if (darkMode > 1) darkMode = 0;
  if (sunlightFadingFix > 1) sunlightFadingFix = 0;
  if (antiGhostingExperimental > 1) antiGhostingExperimental = 0;
  if (x3ReinforceReader > 1) x3ReinforceReader = 0;
  if (x3ReinforceUi > 1) x3ReinforceUi = 0;
  if (x3ReinforceThumbnails > 1) x3ReinforceThumbnails = 0;
  if (x3ReinforcePeriodicClean > 1) x3ReinforcePeriodicClean = 1;
  if (x3ReinforceCleanInterval >= X3_REINFORCE_CLEAN_INTERVAL_COUNT) x3ReinforceCleanInterval = X3_REINFORCE_CLEAN_30;
  if (x3PageWaveform >= X3_PAGE_WAVEFORM_COUNT) x3PageWaveform = X3_PAGE_WAVEFORM_REINFORCE;
  if (x3MaintenanceAction >= X3_MAINTENANCE_ACTION_COUNT) x3MaintenanceAction = X3_MAINTENANCE_FULL_CLEAN;
  if (x3MaintenancePasses >= X3_MAINTENANCE_PASSES_COUNT) x3MaintenancePasses = 0;
  if (tab2Content >= TAB2_CONTENT_COUNT) tab2Content = TAB2_NEWS;
  if (appDrawerNews > 1) appDrawerNews = 1;
  if (appDrawerGames > 1) appDrawerGames = 1;
  if (appDrawerCrossPlay > 1) appDrawerCrossPlay = 1;
  if (appDrawerAgentIsland > 1) appDrawerAgentIsland = 1;
  sleepImageRotationMinutes = normalizeSleepImageRotationMinutes(sleepImageRotationMinutes);
  if (sleepImageRotationEnabled > 1) sleepImageRotationEnabled = 1;
  if (sleepImagePowerDoublePress > 1) {
    sleepImagePowerDoublePress = sleepImagePowerDoublePress == LEGACY_SLEEP_IMAGE_ADVANCE_POWER ? 1 : 0;
  }
  if (powerWakeGuard >= POWER_WAKE_GUARD_COUNT) powerWakeGuard = POWER_WAKE_GUARD_OFF;
  if (sleepImagePowerGestureWindow > 17) sleepImagePowerGestureWindow = 0;
  if (sleepImagePowerFirstPressMin > 15) sleepImagePowerFirstPressMin = 0;
  if (sleepImagePowerSecondPressMax > 9) sleepImagePowerSecondPressMax = 2;
  if (persistentSleepLogs > 1) persistentSleepLogs = 1;
  if (statusBarInnerLeft >= STATUS_BAR_ITEM_COUNT) statusBarInnerLeft = STATUS_ITEM_NONE;
  if (statusBarInnerRight >= STATUS_BAR_ITEM_COUNT) statusBarInnerRight = STATUS_ITEM_NONE;
  if (newsDownloadHour > 23) newsDownloadHour = 6;
  if (newsAutoDownload > 1) newsAutoDownload = 0;
}

uint8_t SystemSetting::getSleepImageRotationMinutes() const {
  return normalizeSleepImageRotationMinutes(sleepImageRotationMinutes);
}

uint16_t SystemSetting::getPowerWakeGuardMs() const {
  if (powerWakeGuard == POWER_WAKE_GUARD_OFF || powerWakeGuard >= POWER_WAKE_GUARD_COUNT) {
    return 0;
  }
  return static_cast<uint16_t>(300 + static_cast<uint16_t>(powerWakeGuard) * 100);
}

uint16_t SystemSetting::getSleepImagePowerGestureWindowMs() const {
  if (sleepImagePowerGestureWindow == 0 || sleepImagePowerGestureWindow > 17) {
    return 0;
  }
  return static_cast<uint16_t>(500 + static_cast<uint16_t>(sleepImagePowerGestureWindow) * 100);
}

uint16_t SystemSetting::getSleepImagePowerFirstPressMinMs() const {
  return sleepImagePowerFirstPressMin <= 15 ? static_cast<uint16_t>(sleepImagePowerFirstPressMin) * 100 : 0;
}

uint16_t SystemSetting::getSleepImagePowerSecondPressMaxMs() const {
  const uint8_t index = sleepImagePowerSecondPressMax <= 9 ? sleepImagePowerSecondPressMax : 2;
  return static_cast<uint16_t>(index + 1) * 100;
}

int SystemSetting::getX3ReinforceCleanInterval() const {
  switch (x3ReinforceCleanInterval) {
    case X3_REINFORCE_CLEAN_10:
      return 10;
    case X3_REINFORCE_CLEAN_15:
      return 15;
    case X3_REINFORCE_CLEAN_30:
    default:
      return 30;
    case X3_REINFORCE_CLEAN_60:
      return 60;
  }
}

const char* SystemSetting::getDeviceName() const {
  const char* name = deviceName;
  while (*name == ' ') {
    ++name;
  }
  return *name == '\0' ? "xteink" : name;
}

std::string SystemSetting::getDeviceHostname() const {
  // mDNS labels are letters, digits and hyphens; anything else becomes a hyphen and runs are
  // collapsed, so "Nick's X3" resolves as nick-s-x3.local rather than not resolving at all.
  std::string host;
  bool lastWasHyphen = true;  // also suppresses a leading hyphen
  for (const char* p = getDeviceName(); *p != '\0' && host.size() < 32; ++p) {
    const auto c = static_cast<unsigned char>(*p);
    if (std::isalnum(c)) {
      host.push_back(static_cast<char>(std::tolower(c)));
      lastWasHyphen = false;
    } else if (!lastWasHyphen) {
      host.push_back('-');
      lastWasHyphen = true;
    }
  }
  while (!host.empty() && host.back() == '-') {
    host.pop_back();
  }
  return host.empty() ? "xteink" : host;
}

namespace {
/**
 * E-inx 1.17.x turned pages with readerDirectionMapping, the power button with readerShortPwrBtn and
 * long presses with longPressChapterSkip. Upstream Inx 1.0.19 replaced all three with per-button
 * bindings, so translate them once, here, so the buttons do what they did before the update.
 */
void migrateLegacyButtonsToBindings(ReaderSetting& reader) {
  using S = SystemSetting;
  switch (reader.readerShortPwrBtn) {
    case S::READER_PAGE_TURN:
      reader.btnPowerShortAction = S::BTN_ACTION_PAGE_NEXT;
      break;
    case S::READER_ANNOTATE:
      reader.btnPowerShortAction = S::BTN_ACTION_ANNOTATE;
      break;
    case S::READER_DICTIONARY:
      reader.btnPowerShortAction = S::BTN_ACTION_DICTIONARY;
      break;
    default:
      reader.btnPowerShortAction = S::BTN_ACTION_PAGE_REFRESH;
      break;
  }

  // Reversed directions swap which button of the pair goes forward, and its chapter skip with it.
  if (reader.readerDirectionMapping == S::MAP_RIGHT_LEFT) {
    reader.btnLeftShortAction = S::BTN_ACTION_PAGE_NEXT;
    reader.btnRightShortAction = S::BTN_ACTION_PAGE_PREVIOUS;
    reader.btnLeftLongAction = S::BTN_ACTION_CHAPTER_SKIP_NEXT;
    reader.btnRightLongAction = S::BTN_ACTION_CHAPTER_SKIP_PREVIOUS;
  } else if (reader.readerDirectionMapping == S::MAP_DOWN_UP) {
    reader.btnUpShortAction = S::BTN_ACTION_PAGE_NEXT;
    reader.btnDownShortAction = S::BTN_ACTION_PAGE_PREVIOUS;
  }

  if (reader.longPressChapterSkip == S::LONG_PRESS_OFF) {
    reader.btnLeftLongAction = S::BTN_ACTION_NONE;
    reader.btnRightLongAction = S::BTN_ACTION_NONE;
  }
}
}  // namespace

/**
 * Reads an E-inx settings file written before the reader settings moved out (versions up to 43, the
 * 1.17.8 layout), into SETTINGS and READER_SETTINGS, then saves both in the current layout. Upstream
 * Inx discards pre-split files and starts from defaults; E-inx keeps the person's settings instead.
 * Delete this once no reader can still have a v43 file.
 */
bool SystemSetting::loadLegacyEinxSettings() {
  ReaderSetting& reader = READER_SETTINGS;
  FsFile inputFile;
  if (!SdMan.openFileForRead("CPS", SETTINGS_FILE, inputFile)) {
    return false;
  }
  uint8_t version = 0;
  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, version);
  serialization::readPod(inputFile, fileSettingsCount);
  uint8_t settingsRead = 0;

  do {
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, reader.extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.statusBar, STATUS_BAR_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    {
      uint8_t rawFontFamily = 0;
      serialization::readPod(inputFile, rawFontFamily);
      reader.fontFamily = rawFontFamily;
#ifndef INX_SIMULATOR_WEB_ONLY
      FontManager::clampReaderFontFamilySlot(reader.fontFamily);
#endif
    }
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.fontSize, FONT_SIZE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    // This slot historically held the lineSpacing enum (0-4). It now holds a numeric line height
    // percentage (10-200). Values below 10 are legacy enums and migrate to the default 100.
    serialization::readPod(inputFile, reader.lineHeight);
    if (reader.lineHeight < 10 || reader.lineHeight > 200) reader.lineHeight = 100;
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepTimeout, SLEEP_TIMEOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, reader.screenMargin);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepScreenCoverMode, SLEEP_SCREEN_COVER_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    {
      std::string urlStr;
      serialization::readString(inputFile, urlStr);
      strncpy(opdsServerUrl, urlStr.c_str(), sizeof(opdsServerUrl) - 1);
      opdsServerUrl[sizeof(opdsServerUrl) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, reader.textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, reader.longPressChapterSkip);
    if (reader.longPressChapterSkip > LONG_PRESS_PAGE_SKIP_5) {
      reader.longPressChapterSkip = LONG_PRESS_CHAPTER_SKIP;
    }
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, reader.hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.readerShortPwrBtn, READER_SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    {
      std::string usernameStr;
      serialization::readString(inputFile, usernameStr);
      strncpy(opdsUsername, usernameStr.c_str(), sizeof(opdsUsername) - 1);
      opdsUsername[sizeof(opdsUsername) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;

    {
      std::string passwordStr;
      serialization::readString(inputFile, passwordStr);
      strncpy(opdsPassword, passwordStr.c_str(), sizeof(opdsPassword) - 1);
      opdsPassword[sizeof(opdsPassword) - 1] = '\0';
    }
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepScreenCoverFilter, SLEEP_SCREEN_COVER_FILTER_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, useLibraryIndex);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, recentLibraryMode, RECENT_LIBRARY_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.readerDirectionMapping, READER_DIRECTION_MAPPING_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.readerMenuButton, READER_MENU_BUTTON_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, bootSetting, BOOT_SETTING_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.statusBarLeft, STATUS_BAR_ITEM_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.statusBarMiddle, STATUS_BAR_ITEM_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, reader.statusBarRight, STATUS_BAR_ITEM_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, reader.pageAutoTurnSeconds);
    if (reader.pageAutoTurnSeconds > 60 || reader.pageAutoTurnSeconds % 10 != 0) {
      reader.pageAutoTurnSeconds = 0;
    }
    if (++settingsRead >= fileSettingsCount) break;

    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, reader.readerImageGrayscale);
      if (reader.readerImageGrayscale >= READER_IMAGE_QUALITY_COUNT) {
        reader.readerImageGrayscale = READER_IMAGE_MEDIUM;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, reader.readerSmartRefreshOnImages);
      if (reader.readerSmartRefreshOnImages > 1) {
        reader.readerSmartRefreshOnImages = 1;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, sleepImageQuality);
      if (sleepImageQuality >= SLEEP_IMAGE_QUALITY_COUNT) {
        sleepImageQuality = SLEEP_IMAGE_HIGH;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      std::string sleepBmpStr;
      serialization::readString(inputFile, sleepBmpStr);
      setSleepCustomBmpFromInput(sleepBmpStr.c_str());
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, reader.legacyReaderImagePresentation, LEGACY_IMAGE_PRESENTATION_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, reader.readerImageDither, READER_IMAGE_DITHER_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, displayImageDither, READER_IMAGE_DITHER_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, legacyDisplayImagePresentation, LEGACY_IMAGE_PRESENTATION_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, reader.paragraphCssIndentEnabled);
      if (reader.paragraphCssIndentEnabled > 1) {
        reader.paragraphCssIndentEnabled = 1;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, refreshOnLoadRecent);
      if (refreshOnLoadRecent > 1) refreshOnLoadRecent = 0;
      ++settingsRead;
      if (settingsRead < fileSettingsCount) {
        serialization::readPod(inputFile, refreshOnLoadLibrary);
        if (refreshOnLoadLibrary > 1) refreshOnLoadLibrary = 0;
        ++settingsRead;
      }
      if (settingsRead < fileSettingsCount) {
        serialization::readPod(inputFile, refreshOnLoadSettings);
        if (refreshOnLoadSettings > 1) refreshOnLoadSettings = 0;
        ++settingsRead;
      }
      if (settingsRead < fileSettingsCount) {
        serialization::readPod(inputFile, refreshOnLoadSync);
        if (refreshOnLoadSync > 1) refreshOnLoadSync = 0;
        ++settingsRead;
      }
      if (settingsRead < fileSettingsCount) {
        serialization::readPod(inputFile, refreshOnLoadStatistics);
        if (refreshOnLoadStatistics > 1) refreshOnLoadStatistics = 0;
        ++settingsRead;
      }
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, bitmapRoundedCorners);
      if (bitmapRoundedCorners > 2) {
        bitmapRoundedCorners = 0;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, recentVisibleCount);
      if (recentVisibleCount < 1 || recentVisibleCount > 9) {
        recentVisibleCount = 9;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, librarySortEnabled);
      if (librarySortEnabled > 1) {
        librarySortEnabled = 1;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, librarySortMode);
      if (librarySortMode > 7) {
        librarySortMode = 0;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, libraryMode, LIBRARY_MODE_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, libraryViewMode, LIBRARY_VIEW_MODE_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, reader.bionicReadingEnabled);
      if (reader.bionicReadingEnabled > 1) {
        reader.bionicReadingEnabled = 0;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, sleepClockStyle, SLEEP_CLOCK_STYLE_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, sleepClockTimeFormat, CLOCK_TIME_FORMAT_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, timeZoneQuarterOffset);
      if (timeZoneQuarterOffset > 104) {
        timeZoneQuarterOffset = 80;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, reader.textSpace);
      if (reader.textSpace < 10 || reader.textSpace > 200) reader.textSpace = 100;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, mainMenuNav, MAIN_MENU_NAV_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, reader.xtcImageQuality, READER_IMAGE_QUALITY_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, reader.xtcShortPwrBtn, XTC_SHORT_PWRBTN_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, reader.xtcPageAutoTurnSeconds);
      if (reader.xtcPageAutoTurnSeconds > 60 || reader.xtcPageAutoTurnSeconds % 10 != 0) {
        reader.xtcPageAutoTurnSeconds = 0;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, reader.xtcRefreshFrequency);
      if (!validRefreshFrequency(reader.xtcRefreshFrequency)) {
        reader.xtcRefreshFrequency = 15;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, sleepClockRefreshInterval, CLOCK_REFRESH_INTERVAL_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, shakePageTurn);
      if (shakePageTurn > 2) shakePageTurn = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, shakePageTurnSensitivity);
      if (shakePageTurnSensitivity > 2) shakePageTurnSensitivity = 1;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, darkMode);
      if (darkMode > 1) darkMode = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, dailyReadingGoal, DAILY_READING_GOAL_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, readerRefreshMode, READER_REFRESH_MODE_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, sunlightFadingFix);
      if (sunlightFadingFix > 1) sunlightFadingFix = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, antiGhostingExperimental);
      if (antiGhostingExperimental > 1) antiGhostingExperimental = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, sleepImageRotationMinutes);
      sleepImageRotationMinutes = normalizeSleepImageRotationMinutes(sleepImageRotationMinutes);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, sleepImageRotationEnabled);
      if (sleepImageRotationEnabled > 1) sleepImageRotationEnabled = 1;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      uint8_t rawSleepImageAdvance = 0;
      serialization::readPod(inputFile, rawSleepImageAdvance);
      sleepImagePowerDoublePress = version < 34 ? (rawSleepImageAdvance == LEGACY_SLEEP_IMAGE_ADVANCE_POWER ? 1 : 0)
                                                : (rawSleepImageAdvance ? 1 : 0);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, powerWakeGuard, POWER_WAKE_GUARD_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, sleepImagePowerGestureWindow);
      if (sleepImagePowerGestureWindow > 17) sleepImagePowerGestureWindow = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, showMenuClock);
      if (showMenuClock > 1) showMenuClock = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, statusBarInnerLeft, STATUS_BAR_ITEM_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, statusBarInnerRight, STATUS_BAR_ITEM_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, sleepImagePowerFirstPressMin);
      if (sleepImagePowerFirstPressMin > 15) sleepImagePowerFirstPressMin = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, sleepImagePowerSecondPressMax);
      if (sleepImagePowerSecondPressMax > 9) sleepImagePowerSecondPressMax = 2;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, persistentSleepLogs);
      if (persistentSleepLogs > 1) persistentSleepLogs = 1;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      std::string newsUrl;
      serialization::readString(inputFile, newsUrl);
      strncpy(newsRepoUrl, newsUrl.c_str(), sizeof(newsRepoUrl) - 1);
      newsRepoUrl[sizeof(newsRepoUrl) - 1] = '\0';
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, newsAutoDownload);
      if (newsAutoDownload > 1) newsAutoDownload = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, newsDownloadHour);
      if (newsDownloadHour > 23) newsDownloadHour = 6;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, uiTheme, UI_THEME_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, libraryShelfEnabled);
      if (libraryShelfEnabled > 1) libraryShelfEnabled = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, x3ReinforceReader);
      if (x3ReinforceReader > 1) x3ReinforceReader = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, x3ReinforceUi);
      if (x3ReinforceUi > 1) x3ReinforceUi = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, x3ReinforceThumbnails);
      if (x3ReinforceThumbnails > 1) x3ReinforceThumbnails = 0;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, x3ReinforcePeriodicClean);
      if (x3ReinforcePeriodicClean > 1) x3ReinforcePeriodicClean = 1;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, x3ReinforceCleanInterval, X3_REINFORCE_CLEAN_INTERVAL_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, tab2Content, TAB2_CONTENT_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, appDrawerNews);
      if (appDrawerNews > 1) appDrawerNews = 1;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, appDrawerGames);
      if (appDrawerGames > 1) appDrawerGames = 1;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, appDrawerCrossPlay);
      if (appDrawerCrossPlay > 1) appDrawerCrossPlay = 1;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, appDrawerAgentIsland);
      if (appDrawerAgentIsland > 1) appDrawerAgentIsland = 1;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      std::string otaUrl;
      serialization::readString(inputFile, otaUrl);
      if (!otaUrl.empty()) {
        strncpy(otaReleaseUrl, otaUrl.c_str(), sizeof(otaReleaseUrl) - 1);
        otaReleaseUrl[sizeof(otaReleaseUrl) - 1] = '\0';
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, x3PageWaveform, X3_PAGE_WAVEFORM_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, x3MaintenanceAction, X3_MAINTENANCE_ACTION_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, x3MaintenancePasses, X3_MAINTENANCE_PASSES_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      std::string name;
      serialization::readString(inputFile, name);
      if (!name.empty()) {
        strncpy(deviceName, name.c_str(), sizeof(deviceName) - 1);
        deviceName[sizeof(deviceName) - 1] = '\0';
      }
      ++settingsRead;
    }

  } while (false);

  inputFile.close();

#ifndef INX_SIMULATOR_WEB_ONLY
  FontManager::clampReaderFontFamilySlot(reader.fontFamily);
#endif

  if (settingsRead < 60) {
    reader.xtcImageQuality = reader.readerImageGrayscale;
  }
  if (settingsRead < 61) {
    reader.xtcShortPwrBtn = reader.readerShortPwrBtn == READER_PAGE_REFRESH ? XTC_POWER_PAGE_REFRESH : XTC_POWER_NEXT;
  }
  if (settingsRead < 62) {
    reader.xtcPageAutoTurnSeconds = reader.pageAutoTurnSeconds;
  }
  if (settingsRead < 63) {
    reader.xtcRefreshFrequency = reader.getRefreshFrequency();
  }
  if (settingsRead < 64) {
    sleepClockRefreshInterval = CLOCK_REFRESH_OFF;
  }
  if (settingsRead < 65) {
    shakePageTurn = 0;
  }
  if (settingsRead < 66) {
    shakePageTurnSensitivity = 1;
  }
  if (settingsRead < 67) {
    darkMode = 0;
  }
  if (settingsRead < 68) {
    dailyReadingGoal = DAILY_GOAL_30_MIN;
  }
  if (settingsRead < 69) {
    readerRefreshMode = READER_REFRESH_AUTO;
  }
  if (settingsRead < 70) {
    sunlightFadingFix = 0;
  }
  if (settingsRead < 71) {
    antiGhostingExperimental = 0;
  }
  if (settingsRead < 72) {
    sleepImageRotationMinutes = 5;
  }
  if (settingsRead < 73) {
    sleepImageRotationEnabled = 1;
  }
  if (settingsRead < 74) {
    sleepImagePowerDoublePress = 0;
  }
  if (settingsRead < 75) {
    powerWakeGuard = POWER_WAKE_GUARD_OFF;
  }
  if (settingsRead < 76) {
    sleepImagePowerGestureWindow = 0;
  }
  if (settingsRead < 77) {
    showMenuClock = 0;
  }
  if (settingsRead < 78) {
    statusBarInnerLeft = STATUS_ITEM_NONE;
  }
  if (settingsRead < 79) {
    statusBarInnerRight = STATUS_ITEM_NONE;
  }
  if (settingsRead < 80) {
    sleepImagePowerFirstPressMin = 0;
  }
  if (settingsRead < 81) {
    sleepImagePowerSecondPressMax = 2;
  }
  if (settingsRead < 82) {
    persistentSleepLogs = 1;
  }
  if (settingsRead < 86) {
    uiTheme = UI_THEME_CLASSIC;
  }
  if (settingsRead < 87) {
    libraryShelfEnabled = 1;
  }
  if (settingsRead < 88) {
    x3ReinforceReader = 0;
  }
  if (settingsRead < 89) {
    x3ReinforceUi = 0;
  }
  if (settingsRead < 90) {
    x3ReinforceThumbnails = 0;
  }
  if (settingsRead < 91) {
    x3ReinforcePeriodicClean = 1;
  }
  if (settingsRead < 92) {
    x3ReinforceCleanInterval = X3_REINFORCE_CLEAN_30;
  }
  if (settingsRead < 93) {
    tab2Content = TAB2_NEWS;
    appDrawerNews = 1;
    appDrawerGames = 1;
    appDrawerCrossPlay = 1;
    appDrawerAgentIsland = 1;
  }

  if (recentVisibleCount < 1 || recentVisibleCount > 9) {
    recentVisibleCount = 9;
  }
  if (librarySortEnabled > 1) {
    librarySortEnabled = 1;
  }
  if (libraryShelfEnabled > 1) {
    libraryShelfEnabled = 0;
  }
  if (librarySortMode > 7) {
    librarySortMode = 0;
  }
  if (sleepClockStyle >= SLEEP_CLOCK_STYLE_COUNT) {
    sleepClockStyle = CLOCK_CENTERED_DATE;
  }
  if (sleepClockTimeFormat >= CLOCK_TIME_FORMAT_COUNT) {
    sleepClockTimeFormat = CLOCK_24_HOUR;
  }
  if (sleepClockRefreshInterval >= CLOCK_REFRESH_INTERVAL_COUNT) {
    sleepClockRefreshInterval = CLOCK_REFRESH_OFF;
  }
  if (sleepImageQuality >= SLEEP_IMAGE_QUALITY_COUNT) {
    sleepImageQuality = SLEEP_IMAGE_HIGH;
  }
  if (reader.xtcImageQuality >= READER_IMAGE_QUALITY_COUNT) {
    reader.xtcImageQuality = READER_IMAGE_LOW;
  }
  if (reader.xtcShortPwrBtn >= XTC_SHORT_PWRBTN_COUNT) {
    reader.xtcShortPwrBtn = XTC_POWER_NEXT;
  }
  if (reader.xtcPageAutoTurnSeconds > 60 || reader.xtcPageAutoTurnSeconds % 10 != 0) {
    reader.xtcPageAutoTurnSeconds = 0;
  }
  if (!validRefreshFrequency(reader.xtcRefreshFrequency)) {
    reader.xtcRefreshFrequency = 15;
  }
  if (timeZoneQuarterOffset > 104) {
    timeZoneQuarterOffset = 80;
  }
  if (libraryMode >= LIBRARY_MODE_COUNT) {
    libraryMode = LIBRARY_GRID;
  }
  if (libraryViewMode >= LIBRARY_VIEW_MODE_COUNT ||
      (libraryViewMode == LIBRARY_VIEW_SHELF && libraryShelfEnabled == 0)) {
    libraryViewMode = LIBRARY_VIEW_FOLDERS;
  }
  if (reader.bionicReadingEnabled > 1) {
    reader.bionicReadingEnabled = 0;
  }
  if (uiTheme >= UI_THEME_COUNT) {
    uiTheme = UI_THEME_CLASSIC;
  }
  loadUiThemeSetting(uiTheme);
  if (dailyReadingGoal >= DAILY_READING_GOAL_COUNT) {
    dailyReadingGoal = DAILY_GOAL_30_MIN;
  }
  if (readerRefreshMode >= READER_REFRESH_MODE_COUNT) {
    readerRefreshMode = READER_REFRESH_AUTO;
  }
  if (sunlightFadingFix > 1) {
    sunlightFadingFix = 0;
  }
  if (antiGhostingExperimental > 1) {
    antiGhostingExperimental = 0;
  }
  if (x3ReinforceReader > 1) {
    x3ReinforceReader = 0;
  }
  if (x3ReinforceUi > 1) {
    x3ReinforceUi = 0;
  }
  if (x3ReinforceThumbnails > 1) {
    x3ReinforceThumbnails = 0;
  }
  if (x3ReinforcePeriodicClean > 1) {
    x3ReinforcePeriodicClean = 1;
  }
  if (x3ReinforceCleanInterval >= X3_REINFORCE_CLEAN_INTERVAL_COUNT) {
    x3ReinforceCleanInterval = X3_REINFORCE_CLEAN_30;
  }
  if (appDrawerNews > 1) appDrawerNews = 1;
  if (appDrawerGames > 1) appDrawerGames = 1;
  if (appDrawerCrossPlay > 1) appDrawerCrossPlay = 1;
  if (appDrawerAgentIsland > 1) appDrawerAgentIsland = 1;
  if (tab2Content >= TAB2_CONTENT_COUNT) {
    tab2Content = TAB2_NEWS;
  }
  sleepImageRotationMinutes = normalizeSleepImageRotationMinutes(sleepImageRotationMinutes);
  if (sleepImageRotationEnabled > 1) {
    sleepImageRotationEnabled = 1;
  }
  if (sleepImagePowerDoublePress > 1) {
    sleepImagePowerDoublePress = sleepImagePowerDoublePress == LEGACY_SLEEP_IMAGE_ADVANCE_POWER ? 1 : 0;
  }
  if (powerWakeGuard >= POWER_WAKE_GUARD_COUNT) {
    powerWakeGuard = POWER_WAKE_GUARD_OFF;
  }
  if (sleepImagePowerGestureWindow > 17) {
    sleepImagePowerGestureWindow = 0;
  }
  if (sleepImagePowerFirstPressMin > 15) {
    sleepImagePowerFirstPressMin = 0;
  }
  if (sleepImagePowerSecondPressMax > 9) {
    sleepImagePowerSecondPressMax = 2;
  }
  if (persistentSleepLogs > 1) {
    persistentSleepLogs = 1;
  }
  if (showMenuClock > 1) {
    showMenuClock = 0;
  }
  if (statusBarInnerLeft >= STATUS_BAR_ITEM_COUNT) {
    statusBarInnerLeft = STATUS_ITEM_NONE;
  }
  if (statusBarInnerRight >= STATUS_BAR_ITEM_COUNT) {
    statusBarInnerRight = STATUS_ITEM_NONE;
  }

  if (settingsRead < LEGACY_SETTINGS_COUNT) {
    if (settingsRead < LEGACY_SETTINGS_COUNT_V9) {
      displayImageDither = reader.readerImageDither;
    }
    legacyDisplayImagePresentation = reader.legacyReaderImagePresentation;
  }

  Serial.printf("[%lu] [CPS] Legacy settings read (version %u, %u items, darkMode=%u)\n", millis(), version,
                settingsRead, darkMode);

  migrateLegacyButtonsToBindings(reader);

  // Always rewrite: this file is in the old layout, and the reader half now lives in its own file.
  saveToFile();
  reader.saveToFile();
  Serial.printf("[%lu] [CPS] Migrated E-inx settings v%u into settings.bin v%u + reader_settings.bin\n", millis(),
                version, SETTINGS_FILE_VERSION);

  return true;
}
