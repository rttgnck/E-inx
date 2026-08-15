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

#include <cstdio>
#include <cstring>
#include <string>

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
constexpr uint8_t SETTINGS_FILE_VERSION = 43;
constexpr uint8_t SETTINGS_COUNT = 101;
/** Last field index in v9 (1-based count of persisted pods through displayImageDither). */
constexpr uint8_t SETTINGS_COUNT_V9 = 40;
constexpr uint8_t LEGACY_SLEEP_IMAGE_ADVANCE_POWER = 7;
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
  if ((strncmp(buf, "/sleep/", 7) == 0 || strncmp(buf, "/Wallpapers/", 12) == 0) && strstr(buf, "..") == nullptr &&
      strchr(buf + 1, ':') == nullptr && strchr(buf + 1, '\\') == nullptr) {
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

uint32_t settingsHash(const SystemSetting& settings, const uint8_t fontFamilyToSave) {
  uint32_t hash = FNV1A_OFFSET;
  hashPod(hash, SETTINGS_FILE_VERSION);
  hashPod(hash, SETTINGS_COUNT);
  hashPod(hash, settings.sleepScreen);
  hashPod(hash, settings.extraParagraphSpacing);
  hashPod(hash, settings.shortPwrBtn);
  hashPod(hash, settings.statusBar);
  hashPod(hash, settings.orientation);
  hashPod(hash, settings.frontButtonLayout);
  hashPod(hash, settings.sideButtonLayout);
  hashPod(hash, fontFamilyToSave);
  hashPod(hash, settings.fontSize);
  hashPod(hash, settings.lineHeight);
  hashPod(hash, settings.paragraphAlignment);
  hashPod(hash, settings.sleepTimeout);
  hashPod(hash, settings.refreshFrequency);
  hashPod(hash, settings.screenMargin);
  hashPod(hash, settings.sleepScreenCoverMode);
  hashString(hash, settings.opdsServerUrl);
  hashPod(hash, settings.textAntiAliasing);
  hashPod(hash, settings.hideBatteryPercentage);
  hashPod(hash, settings.longPressChapterSkip);
  hashPod(hash, settings.hyphenationEnabled);
  hashPod(hash, settings.readerShortPwrBtn);
  hashString(hash, settings.opdsUsername);
  hashString(hash, settings.opdsPassword);
  hashPod(hash, settings.sleepScreenCoverFilter);
  hashPod(hash, settings.useLibraryIndex);
  hashPod(hash, settings.recentLibraryMode);
  hashPod(hash, settings.readerDirectionMapping);
  hashPod(hash, settings.readerMenuButton);
  hashPod(hash, settings.bootSetting);
  hashPod(hash, settings.statusBarLeft);
  hashPod(hash, settings.statusBarMiddle);
  hashPod(hash, settings.statusBarRight);
  hashPod(hash, settings.pageAutoTurnSeconds);
  hashPod(hash, settings.readerImageGrayscale);
  hashPod(hash, settings.readerSmartRefreshOnImages);
  hashPod(hash, settings.sleepImageQuality);
  hashString(hash, settings.sleepCustomBmp);
  hashPod(hash, settings.legacyReaderImagePresentation);
  hashPod(hash, settings.readerImageDither);
  hashPod(hash, settings.displayImageDither);
  hashPod(hash, settings.legacyDisplayImagePresentation);
  hashPod(hash, settings.paragraphCssIndentEnabled);
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
  hashPod(hash, settings.bionicReadingEnabled);
  hashPod(hash, settings.sleepClockStyle);
  hashPod(hash, settings.sleepClockTimeFormat);
  hashPod(hash, settings.timeZoneQuarterOffset);
  hashPod(hash, settings.textSpace);
  hashPod(hash, settings.mainMenuNav);
  hashPod(hash, settings.xtcImageQuality);
  hashPod(hash, settings.xtcShortPwrBtn);
  hashPod(hash, settings.xtcPageAutoTurnSeconds);
  hashPod(hash, settings.xtcRefreshFrequency);
  hashPod(hash, settings.sleepClockRefreshInterval);
  hashPod(hash, settings.shakePageTurn);
  hashPod(hash, settings.shakePageTurnSensitivity);
  hashPod(hash, settings.uiTheme);
  hashPod(hash, settings.libraryShelfEnabled);
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
  hashPod(hash, settings.showBottomBarClock);
  hashPod(hash, settings.statusBarInnerLeft);
  hashPod(hash, settings.statusBarInnerRight);
  hashPod(hash, settings.sleepImagePowerFirstPressMin);
  hashPod(hash, settings.sleepImagePowerSecondPressMax);
  hashPod(hash, settings.persistentSleepLogs);
  hashString(hash, settings.newsRepoUrl);
  hashString(hash, settings.otaReleaseUrl);
  hashPod(hash, settings.newsAutoDownload);
  hashPod(hash, settings.newsDownloadHour);
  hashPod(hash, settings.uiTheme);
  hashPod(hash, settings.libraryShelfEnabled);
  hashPod(hash, settings.x3ReinforceReader);
  hashPod(hash, settings.x3ReinforceUi);
  hashPod(hash, settings.x3ReinforceThumbnails);
  hashPod(hash, settings.x3ReinforcePeriodicClean);
  hashPod(hash, settings.x3ReinforceCleanInterval);
  hashPod(hash, settings.x3PageWaveform);
  hashPod(hash, settings.x3MaintenanceAction);
  hashPod(hash, settings.x3MaintenancePasses);
  hashPod(hash, settings.tab2Content);
  hashPod(hash, settings.appDrawerNews);
  hashPod(hash, settings.appDrawerGames);
  hashPod(hash, settings.appDrawerCrossPlay);
  hashPod(hash, settings.appDrawerAgentIsland);
  return hash;
}
}  // namespace

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
  uint8_t fontFamilyToSave = fontFamily;
#ifndef INX_SIMULATOR_WEB_ONLY
  FontManager::clampReaderFontFamilySlot(fontFamilyToSave);
  if (fontFamilyToSave != fontFamily) {
    const_cast<SystemSetting*>(this)->fontFamily = fontFamilyToSave;
  }
#endif

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
    if (mut->bionicReadingEnabled > 1) mut->bionicReadingEnabled = 0;
    if (mut->sleepClockStyle >= SLEEP_CLOCK_STYLE_COUNT) mut->sleepClockStyle = CLOCK_CENTERED_DATE;
    if (mut->sleepClockTimeFormat >= CLOCK_TIME_FORMAT_COUNT) mut->sleepClockTimeFormat = CLOCK_24_HOUR;
    if (mut->sleepClockRefreshInterval >= CLOCK_REFRESH_INTERVAL_COUNT)
      mut->sleepClockRefreshInterval = CLOCK_REFRESH_OFF;
    if (mut->sleepImageQuality >= SLEEP_IMAGE_QUALITY_COUNT) mut->sleepImageQuality = SLEEP_IMAGE_HIGH;
    if (mut->xtcImageQuality >= READER_IMAGE_QUALITY_COUNT) mut->xtcImageQuality = READER_IMAGE_LOW;
    if (mut->xtcShortPwrBtn >= XTC_SHORT_PWRBTN_COUNT) mut->xtcShortPwrBtn = XTC_POWER_NEXT;
    if (mut->xtcPageAutoTurnSeconds > 60 || mut->xtcPageAutoTurnSeconds % 10 != 0) mut->xtcPageAutoTurnSeconds = 0;
    if (!validRefreshFrequency(mut->xtcRefreshFrequency)) mut->xtcRefreshFrequency = 15;
    if (mut->timeZoneQuarterOffset > 104) mut->timeZoneQuarterOffset = 80;
    if (mut->shakePageTurn > 2) mut->shakePageTurn = 0;
    if (mut->shakePageTurnSensitivity > 2) mut->shakePageTurnSensitivity = 1;
    if (mut->uiTheme >= UI_THEME_COUNT) mut->uiTheme = UI_THEME_CLASSIC;
    if (mut->dailyReadingGoal >= DAILY_READING_GOAL_COUNT) mut->dailyReadingGoal = DAILY_GOAL_30_MIN;
    if (mut->readerRefreshMode >= READER_REFRESH_MODE_COUNT) mut->readerRefreshMode = READER_REFRESH_AUTO;
    if (mut->sunlightFadingFix > 1) mut->sunlightFadingFix = 0;
    if (mut->antiGhostingExperimental > 1) mut->antiGhostingExperimental = 0;
    if (mut->x3ReinforceReader > 1) mut->x3ReinforceReader = 0;
    if (mut->x3ReinforceUi > 1) mut->x3ReinforceUi = 0;
    if (mut->x3ReinforceThumbnails > 1) mut->x3ReinforceThumbnails = 0;
    if (mut->x3ReinforcePeriodicClean > 1) mut->x3ReinforcePeriodicClean = 1;
    if (mut->x3ReinforceCleanInterval >= X3_REINFORCE_CLEAN_INTERVAL_COUNT)
      mut->x3ReinforceCleanInterval = X3_REINFORCE_CLEAN_30;
    if (mut->x3PageWaveform >= X3_PAGE_WAVEFORM_COUNT) mut->x3PageWaveform = X3_PAGE_WAVEFORM_REINFORCE;
    if (mut->x3MaintenanceAction >= X3_MAINTENANCE_ACTION_COUNT)
      mut->x3MaintenanceAction = X3_MAINTENANCE_FULL_CLEAN;
    if (mut->x3MaintenancePasses >= X3_MAINTENANCE_PASSES_COUNT) mut->x3MaintenancePasses = 0;
    if (mut->tab2Content >= TAB2_CONTENT_COUNT) mut->tab2Content = TAB2_NEWS;
    if (mut->appDrawerNews > 1) mut->appDrawerNews = 1;
    if (mut->appDrawerGames > 1) mut->appDrawerGames = 1;
    if (mut->appDrawerCrossPlay > 1) mut->appDrawerCrossPlay = 1;
    if (mut->appDrawerAgentIsland > 1) mut->appDrawerAgentIsland = 1;
    mut->sleepImageRotationMinutes = normalizeSleepImageRotationMinutes(mut->sleepImageRotationMinutes);
    if (mut->sleepImageRotationEnabled > 1) mut->sleepImageRotationEnabled = 1;
    if (mut->sleepImagePowerDoublePress > 1) {
      mut->sleepImagePowerDoublePress = mut->sleepImagePowerDoublePress == LEGACY_SLEEP_IMAGE_ADVANCE_POWER ? 1 : 0;
    }
    if (mut->powerWakeGuard >= POWER_WAKE_GUARD_COUNT) mut->powerWakeGuard = POWER_WAKE_GUARD_OFF;
    if (mut->sleepImagePowerGestureWindow > 17) mut->sleepImagePowerGestureWindow = 0;
    if (mut->sleepImagePowerFirstPressMin > 15) mut->sleepImagePowerFirstPressMin = 0;
    if (mut->sleepImagePowerSecondPressMax > 9) mut->sleepImagePowerSecondPressMax = 2;
    if (mut->persistentSleepLogs > 1) mut->persistentSleepLogs = 1;
    if (mut->showBottomBarClock > 1) mut->showBottomBarClock = 0;
    if (mut->statusBarInnerLeft >= STATUS_BAR_ITEM_COUNT) mut->statusBarInnerLeft = STATUS_ITEM_NONE;
    if (mut->statusBarInnerRight >= STATUS_BAR_ITEM_COUNT) mut->statusBarInnerRight = STATUS_ITEM_NONE;
  }

  const uint32_t currentHash = settingsHash(*this, fontFamilyToSave);
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
  serialization::writePod(outputFile, extraParagraphSpacing);
  serialization::writePod(outputFile, shortPwrBtn);
  serialization::writePod(outputFile, statusBar);
  serialization::writePod(outputFile, orientation);
  serialization::writePod(outputFile, frontButtonLayout);
  serialization::writePod(outputFile, sideButtonLayout);
  serialization::writePod(outputFile, fontFamilyToSave);
  serialization::writePod(outputFile, fontSize);
  serialization::writePod(outputFile, lineHeight);
  serialization::writePod(outputFile, paragraphAlignment);
  serialization::writePod(outputFile, sleepTimeout);
  serialization::writePod(outputFile, refreshFrequency);
  serialization::writePod(outputFile, screenMargin);
  serialization::writePod(outputFile, sleepScreenCoverMode);
  serialization::writeString(outputFile, std::string(opdsServerUrl));
  serialization::writePod(outputFile, textAntiAliasing);
  serialization::writePod(outputFile, hideBatteryPercentage);
  serialization::writePod(outputFile, longPressChapterSkip);
  serialization::writePod(outputFile, hyphenationEnabled);
  serialization::writePod(outputFile, readerShortPwrBtn);
  serialization::writeString(outputFile, std::string(opdsUsername));
  serialization::writeString(outputFile, std::string(opdsPassword));
  serialization::writePod(outputFile, sleepScreenCoverFilter);
  serialization::writePod(outputFile, useLibraryIndex);
  serialization::writePod(outputFile, recentLibraryMode);
  serialization::writePod(outputFile, readerDirectionMapping);
  serialization::writePod(outputFile, readerMenuButton);
  serialization::writePod(outputFile, bootSetting);
  serialization::writePod(outputFile, statusBarLeft);
  serialization::writePod(outputFile, statusBarMiddle);
  serialization::writePod(outputFile, statusBarRight);
  serialization::writePod(outputFile, pageAutoTurnSeconds);
  serialization::writePod(outputFile, readerImageGrayscale);
  serialization::writePod(outputFile, readerSmartRefreshOnImages);
  serialization::writePod(outputFile, sleepImageQuality);
  serialization::writeString(outputFile, std::string(sleepCustomBmp));
  serialization::writePod(outputFile, legacyReaderImagePresentation);
  serialization::writePod(outputFile, readerImageDither);
  serialization::writePod(outputFile, displayImageDither);
  serialization::writePod(outputFile, legacyDisplayImagePresentation);
  serialization::writePod(outputFile, paragraphCssIndentEnabled);
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
  serialization::writePod(outputFile, bionicReadingEnabled);
  serialization::writePod(outputFile, sleepClockStyle);
  serialization::writePod(outputFile, sleepClockTimeFormat);
  serialization::writePod(outputFile, timeZoneQuarterOffset);
  serialization::writePod(outputFile, textSpace);
  serialization::writePod(outputFile, mainMenuNav);
  serialization::writePod(outputFile, xtcImageQuality);
  serialization::writePod(outputFile, xtcShortPwrBtn);
  serialization::writePod(outputFile, xtcPageAutoTurnSeconds);
  serialization::writePod(outputFile, xtcRefreshFrequency);
  serialization::writePod(outputFile, sleepClockRefreshInterval);
  serialization::writePod(outputFile, shakePageTurn);
  serialization::writePod(outputFile, shakePageTurnSensitivity);
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
  serialization::writePod(outputFile, showBottomBarClock);
  serialization::writePod(outputFile, statusBarInnerLeft);
  serialization::writePod(outputFile, statusBarInnerRight);
  serialization::writePod(outputFile, sleepImagePowerFirstPressMin);
  serialization::writePod(outputFile, sleepImagePowerSecondPressMax);
  serialization::writePod(outputFile, persistentSleepLogs);
  serialization::writeString(outputFile, std::string(newsRepoUrl));
  serialization::writePod(outputFile, newsAutoDownload);
  serialization::writePod(outputFile, newsDownloadHour);
  serialization::writePod(outputFile, uiTheme);
  serialization::writePod(outputFile, libraryShelfEnabled);
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
// cppcheck-suppress checkLevelNormal ; large versioned-field function, exhaustive level not worth the CI runtime cost
bool SystemSetting::loadFromFile() {
  FsFile inputFile;

  if (!SdMan.openFileForRead("CPS", SETTINGS_FILE, inputFile)) {
    statusBarLeft = STATUS_ITEM_BATTERY_ICON_WITH_PERCENT;
    statusBarMiddle = STATUS_ITEM_CHAPTER_TITLE;
    statusBarRight = STATUS_ITEM_PAGE_NUMBERS;
    saveToFile();
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);

  if (version > SETTINGS_FILE_VERSION) {
    Serial.printf("[%lu] [CPS] Deserialization failed: Unknown version %u (expected <= %u)\n", millis(), version,
                  SETTINGS_FILE_VERSION);
    inputFile.close();
    statusBarLeft = STATUS_ITEM_BATTERY_ICON_WITH_PERCENT;
    statusBarMiddle = STATUS_ITEM_CHAPTER_TITLE;
    statusBarRight = STATUS_ITEM_PAGE_NUMBERS;
    return false;
  }

  uint8_t fileSettingsCount = 0;
  serialization::readPod(inputFile, fileSettingsCount);
  const bool shouldRewriteSettings = version < SETTINGS_FILE_VERSION || fileSettingsCount < SETTINGS_COUNT;
  uint8_t settingsRead = 0;

  do {
    readAndValidate(inputFile, sleepScreen, SLEEP_SCREEN_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, extraParagraphSpacing);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, shortPwrBtn, SHORT_PWRBTN_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, statusBar, STATUS_BAR_MODE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, orientation, ORIENTATION_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, frontButtonLayout, FRONT_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sideButtonLayout, SIDE_BUTTON_LAYOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    {
      uint8_t rawFontFamily = 0;
      serialization::readPod(inputFile, rawFontFamily);
      fontFamily = rawFontFamily;
#ifndef INX_SIMULATOR_WEB_ONLY
      FontManager::clampReaderFontFamilySlot(fontFamily);
#endif
    }
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, fontSize, FONT_SIZE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    // This slot historically held the lineSpacing enum (0-4). It now holds a numeric line height
    // percentage (10-200). Values below 10 are legacy enums and migrate to the default 100.
    serialization::readPod(inputFile, lineHeight);
    if (lineHeight < 10 || lineHeight > 200) lineHeight = 100;
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, paragraphAlignment, PARAGRAPH_ALIGNMENT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, sleepTimeout, SLEEP_TIMEOUT_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, refreshFrequency, REFRESH_FREQUENCY_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, screenMargin);
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

    serialization::readPod(inputFile, textAntiAliasing);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, hideBatteryPercentage, HIDE_BATTERY_PERCENTAGE_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, longPressChapterSkip);
    if (longPressChapterSkip > LONG_PRESS_PAGE_SKIP_5) {
      longPressChapterSkip = LONG_PRESS_CHAPTER_SKIP;
    }
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, hyphenationEnabled);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, readerShortPwrBtn, READER_SHORT_PWRBTN_COUNT);
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

    readAndValidate(inputFile, readerDirectionMapping, READER_DIRECTION_MAPPING_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, readerMenuButton, READER_MENU_BUTTON_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, bootSetting, BOOT_SETTING_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, statusBarLeft, STATUS_BAR_ITEM_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, statusBarMiddle, STATUS_BAR_ITEM_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    readAndValidate(inputFile, statusBarRight, STATUS_BAR_ITEM_COUNT);
    if (++settingsRead >= fileSettingsCount) break;

    serialization::readPod(inputFile, pageAutoTurnSeconds);
    if (pageAutoTurnSeconds > 60 || pageAutoTurnSeconds % 10 != 0) {
      pageAutoTurnSeconds = 0;
    }
    if (++settingsRead >= fileSettingsCount) break;

    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, readerImageGrayscale);
      if (readerImageGrayscale >= READER_IMAGE_QUALITY_COUNT) {
        readerImageGrayscale = READER_IMAGE_MEDIUM;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, readerSmartRefreshOnImages);
      if (readerSmartRefreshOnImages > 1) {
        readerSmartRefreshOnImages = 1;
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
      readAndValidate(inputFile, legacyReaderImagePresentation, LEGACY_IMAGE_PRESENTATION_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, readerImageDither, READER_IMAGE_DITHER_COUNT);
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
      serialization::readPod(inputFile, paragraphCssIndentEnabled);
      if (paragraphCssIndentEnabled > 1) {
        paragraphCssIndentEnabled = 1;
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
      serialization::readPod(inputFile, bionicReadingEnabled);
      if (bionicReadingEnabled > 1) {
        bionicReadingEnabled = 0;
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
      serialization::readPod(inputFile, textSpace);
      if (textSpace < 10 || textSpace > 200) textSpace = 100;
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, mainMenuNav, MAIN_MENU_NAV_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, xtcImageQuality, READER_IMAGE_QUALITY_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      readAndValidate(inputFile, xtcShortPwrBtn, XTC_SHORT_PWRBTN_COUNT);
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, xtcPageAutoTurnSeconds);
      if (xtcPageAutoTurnSeconds > 60 || xtcPageAutoTurnSeconds % 10 != 0) {
        xtcPageAutoTurnSeconds = 0;
      }
      ++settingsRead;
    }
    if (settingsRead < fileSettingsCount) {
      serialization::readPod(inputFile, xtcRefreshFrequency);
      if (!validRefreshFrequency(xtcRefreshFrequency)) {
        xtcRefreshFrequency = 15;
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
      serialization::readPod(inputFile, showBottomBarClock);
      if (showBottomBarClock > 1) showBottomBarClock = 0;
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

  } while (false);

  inputFile.close();

#ifndef INX_SIMULATOR_WEB_ONLY
  FontManager::clampReaderFontFamilySlot(fontFamily);
#endif

  if (settingsRead < 60) {
    xtcImageQuality = readerImageGrayscale;
  }
  if (settingsRead < 61) {
    xtcShortPwrBtn = readerShortPwrBtn == READER_PAGE_REFRESH ? XTC_POWER_PAGE_REFRESH : XTC_POWER_NEXT;
  }
  if (settingsRead < 62) {
    xtcPageAutoTurnSeconds = pageAutoTurnSeconds;
  }
  if (settingsRead < 63) {
    xtcRefreshFrequency = getRefreshFrequency();
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
    showBottomBarClock = 0;
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
  if (xtcImageQuality >= READER_IMAGE_QUALITY_COUNT) {
    xtcImageQuality = READER_IMAGE_LOW;
  }
  if (xtcShortPwrBtn >= XTC_SHORT_PWRBTN_COUNT) {
    xtcShortPwrBtn = XTC_POWER_NEXT;
  }
  if (xtcPageAutoTurnSeconds > 60 || xtcPageAutoTurnSeconds % 10 != 0) {
    xtcPageAutoTurnSeconds = 0;
  }
  if (!validRefreshFrequency(xtcRefreshFrequency)) {
    xtcRefreshFrequency = 15;
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
  if (bionicReadingEnabled > 1) {
    bionicReadingEnabled = 0;
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
  if (showBottomBarClock > 1) {
    showBottomBarClock = 0;
  }
  if (statusBarInnerLeft >= STATUS_BAR_ITEM_COUNT) {
    statusBarInnerLeft = STATUS_ITEM_NONE;
  }
  if (statusBarInnerRight >= STATUS_BAR_ITEM_COUNT) {
    statusBarInnerRight = STATUS_ITEM_NONE;
  }

  if (settingsRead < SETTINGS_COUNT) {
    if (settingsRead < SETTINGS_COUNT_V9) {
      displayImageDither = readerImageDither;
    }
    legacyDisplayImagePresentation = legacyReaderImagePresentation;
  }

  Serial.printf("[%lu] [CPS] Settings loaded (version %u, %u items, darkMode=%u)\n", millis(), version, settingsRead,
                darkMode);

  if (shouldRewriteSettings) {
    saveToFile();
  }

  return true;
}

/**
 * @brief Gets reader line compression factor based on font and spacing
 * @return Line compression multiplier
 */
float SystemSetting::getReaderLineCompression() const {
  // lineHeight is a percentage of the font's natural line height (100 = normal). Clamp 10-200.
  uint8_t lh = lineHeight;
  if (lh < 10 || lh > 200) lh = 100;
  return static_cast<float>(lh) / 100.0f;
}

float SystemSetting::getReaderWordSpacingFactor() const {
  // textSpace is a percentage of the natural inter-word space (100 = normal). Clamp 10-200.
  uint8_t ts = textSpace;
  if (ts < 10 || ts > 200) ts = 100;
  return static_cast<float>(ts) / 100.0f;
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

/**
 * @brief Gets screen refresh frequency in pages
 * @return Number of pages between refreshes
 */
int SystemSetting::getRefreshFrequency() const {
  switch (refreshFrequency) {
    case REFRESH_1:
      return 1;
    case REFRESH_5:
      return 5;
    case REFRESH_10:
      return 10;
    case REFRESH_15:
    default:
      return 15;
    case REFRESH_30:
      return 30;
  }
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

int SystemSetting::getReaderFontIdForSettingsUi(uint8_t familySlot, uint8_t sizeIndex) const {
#ifdef INX_SIMULATOR_WEB_ONLY
  (void)familySlot;
  (void)sizeIndex;
  return 0;
#else
  if (familySlot < FONT_FAMILY_BUILTIN_COUNT) {
    return getReaderFontIdForFamilyAndSize(familySlot, sizeIndex);
  }
  return getReaderFontIdForFamilyAndSize(ATKINSON_HYPERLEGIBLE, sizeIndex);
#endif
}

int SystemSetting::getReaderFontIdForFamilyAndSize(uint8_t family, uint8_t size) const {
#ifdef INX_SIMULATOR_WEB_ONLY
  (void)family;
  (void)size;
  return 0;
#else
  if (size >= FONT_SIZE_COUNT) {
    size = MEDIUM;
  }
  static const int kPtBySize[] = {10, 12, 14, 16, 18};
  const int preferredPt = kPtBySize[size];

  if (family >= FONT_FAMILY_BUILTIN_COUNT) {
    const std::string sdName = FontManager::readerFontFamilyLabel(family);
    if (sdName == "Literata" || sdName == "Atkinson Hyperlegible") {
      return getReaderFontIdForFamilyAndSize(sdName == "Atkinson Hyperlegible" ? ATKINSON_HYPERLEGIBLE : LITERATA,
                                             size);
    }
    return FontManager::getFontIdNearestPointSize(sdName, preferredPt);
  }

  switch (family) {
    case ATKINSON_HYPERLEGIBLE:
      switch (size) {
        case EXTRA_SMALL:
          return ATKINSON_HYPERLEGIBLE_10_FONT_ID;
        case SMALL:
          return ATKINSON_HYPERLEGIBLE_12_FONT_ID;
        case MEDIUM:
        default:
          return ATKINSON_HYPERLEGIBLE_14_FONT_ID;
        case LARGE:
          return ATKINSON_HYPERLEGIBLE_16_FONT_ID;
        case EXTRA_LARGE:
          return ATKINSON_HYPERLEGIBLE_18_FONT_ID;
      }
    case LITERATA:
    default:
      switch (size) {
        case EXTRA_SMALL:
          return LITERATA_10_FONT_ID;
        case SMALL:
          return LITERATA_12_FONT_ID;
        case MEDIUM:
        default:
          return LITERATA_14_FONT_ID;
        case LARGE:
          return LITERATA_16_FONT_ID;
        case EXTRA_LARGE:
          return LITERATA_18_FONT_ID;
      }
  }
#endif
}

/**
 * @brief Gets reader font ID based on font family and size
 * @return Font identifier for rendering
 */
int SystemSetting::getReaderFontId() const {
#ifdef INX_SIMULATOR_WEB_ONLY
  return 0;
#else
  return getReaderFontIdForFamilyAndSize(fontFamily, fontSize);
#endif
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
