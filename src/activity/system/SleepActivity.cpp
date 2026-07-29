/**
 * @file SleepActivity.cpp
 * @brief Definitions for SleepActivity.
 */

#include "SleepActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <ImageDisplayCache.h>
#include <ImageRender.h>
#include <SDCardManager.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <vector>

#include "../reader/Epub/StatusBar.h"
#include "images/CorgiSleep.h"
#include "state/BookProgress.h"
#include "state/BookSetting.h"
#include "state/RecentBooks.h"
#include "state/Session.h"
#include "state/SleepImageSelection.h"
#include "state/SystemSetting.h"
#include "system/FontManager.h"
#include "system/Fonts.h"
#include "system/ScreenComponents.h"
#include "system/SleepClockRenderer.h"
#include "util/StringUtils.h"

namespace {
class SleepImageToneGuard {
 public:
  explicit SleepImageToneGuard(GfxRenderer& renderer)
      : renderer_(renderer), previous_(renderer.setPreserveImageTone(true)) {}
  ~SleepImageToneGuard() { renderer_.setPreserveImageTone(previous_); }

 private:
  GfxRenderer& renderer_;
  bool previous_;
};

bool isSleepImagePathJpeg(const std::string& path) {
  return StringUtils::checkFileExtension(path, ".jpg") || StringUtils::checkFileExtension(path, ".jpeg");
}
bool isSupportedSleepImageFile(const std::string& filename) {
  return StringUtils::checkFileExtension(filename, ".bmp") || StringUtils::checkFileExtension(filename, ".jpg") ||
         StringUtils::checkFileExtension(filename, ".jpeg");
}

bool sleepTwoBitEnabled() {
  return SETTINGS.sleepImageQuality != SystemSetting::SLEEP_IMAGE_LOW &&
         SETTINGS.sleepScreenCoverFilter == SystemSetting::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
}

bool sleepImageQualityEnabled() { return SETTINGS.sleepImageQuality == SystemSetting::SLEEP_IMAGE_HIGH; }

bool dateTimeSleepScreenAvailable() { return gpio.deviceIsX3(); }

ImageRenderMode sleepImageRenderMode() {
  return sleepTwoBitEnabled() ? ImageRenderMode::TwoBit : ImageRenderMode::OneBit;
}

ImageRender::Options sleepImageOptions(const bool allowQuality = true) {
  ImageRender::Options options;
  options.cropToFill = SETTINGS.sleepScreenCoverMode == SystemSetting::SLEEP_SCREEN_COVER_MODE::FIT;
  options.mode = sleepImageRenderMode();
  options.useDisplayCache = true;
  options.quality = allowQuality && sleepImageQualityEnabled();
  options.fastQuality = false;
  return options;
}

bool runSleepImageTwoBitPasses(GfxRenderer& renderer, const std::string& imagePath,
                               const ImageRender::Options& baseOptions, const bool allowQuality = true) {
  if (!sleepTwoBitEnabled()) {
    return true;
  }

  ImageRender::Options options = baseOptions;
  options.mode = ImageRenderMode::TwoBit;
  options.useDisplayCache = true;
  // Transparent overlays cap at MEDIUM (allowQuality=false): the HIGH quality LUT can't composite over existing
  // content, so HIGH transparent removes the background instead (see renderTransparentSleepScreen).
  const bool quality = allowQuality && sleepImageQualityEnabled();
  options.quality = quality;
  options.fastQuality = false;

  return ImageRender::create(renderer, imagePath)
      .displayGrayscale(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), options, quality);
}

void recordSleepImageUsed(const std::string& imagePath) {
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 220, APP_STATE.lastSleepImage);
  APP_STATE.lastSleepImagePath = imagePath;
  APP_STATE.lastSleepImage++;
  APP_STATE.saveToFile();
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 221, APP_STATE.lastSleepImage);
}

void renderSleepImageDiagnostic(GfxRenderer& renderer, const char* message) {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int font = ATKINSON_HYPERLEGIBLE_12_FONT_ID;
  const int lineHeight = renderer.text.getLineHeight(font);
  const std::string text = renderer.text.truncate(font, message, pageWidth - 40, EpdFontFamily::BOLD);
  renderer.text.render(font, 20, (pageHeight - lineHeight) / 2, text.c_str(), true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

bool randomSleepImageEnabled() { return SETTINGS.sleepCustomBmp[0] == '\0'; }

std::string pathForFixedSleepBmp() {
  if (SETTINGS.sleepCustomBmp[0] == '\0') {
    return "";
  }
  if (strcmp(SETTINGS.sleepCustomBmp, "/sleep.bmp") == 0) return SdMan.exists("/sleep.bmp") ? "/sleep.bmp" : "";
  if (strcmp(SETTINGS.sleepCustomBmp, "/sleep.jpg") == 0) return SdMan.exists("/sleep.jpg") ? "/sleep.jpg" : "";
  if (strcmp(SETTINGS.sleepCustomBmp, "/sleep.jpeg") == 0) return SdMan.exists("/sleep.jpeg") ? "/sleep.jpeg" : "";
  if (strncmp(SETTINGS.sleepCustomBmp, "/sleep/", 7) == 0 ||
      strncmp(SETTINGS.sleepCustomBmp, "/Wallpapers/", 12) == 0) {
    return SdMan.exists(SETTINGS.sleepCustomBmp) ? SETTINGS.sleepCustomBmp : "";
  }
  const std::string path = std::string("/sleep/") + SETTINGS.sleepCustomBmp;
  if (SdMan.exists(path.c_str())) {
    return path;
  }
  return "";
}

std::vector<std::string> listSleepImagePaths() {
  std::vector<std::string> paths;
  const char* folders[] = {"/sleep", "/Wallpapers"};

  for (const char* folder : folders) {
    auto dir = SdMan.open(folder);
    if (dir && dir.isDirectory()) {
      char name[256];
      while (auto file = dir.openNextFile()) {
        file.getName(name, sizeof(name));
        std::string filename = name;
        if (filename[0] != '.' && isSupportedSleepImageFile(filename)) {
          const std::string path = std::string(folder) + "/" + filename;
          if (isSleepImageShuffleEnabled(path)) {
            paths.push_back(path);
          }
        }
        file.close();
      }
      dir.close();
    }
  }

  std::sort(paths.begin(), paths.end());
  return paths;
}

size_t randomSleepImagePoolSize() {
  const std::vector<std::string> sleepImages = listSleepImagePaths();
  if (!sleepImages.empty()) {
    return sleepImages.size();
  }

  size_t fallbackCount = 0;
  if (SdMan.exists("/sleep.bmp") && isSleepImageShuffleEnabled("/sleep.bmp")) {
    fallbackCount++;
  }
  if (SdMan.exists("/sleep.jpg") && isSleepImageShuffleEnabled("/sleep.jpg")) {
    fallbackCount++;
  }
  if (SdMan.exists("/sleep.jpeg") && isSleepImageShuffleEnabled("/sleep.jpeg")) {
    fallbackCount++;
  }
  return fallbackCount;
}

void purgeSleepImageDisplayCaches(GfxRenderer& renderer, const std::string& imagePath) {
  if (imagePath.empty()) {
    return;
  }

  constexpr bool cropModes[] = {false, true};
  for (const bool cropToFill : cropModes) {
    ImageDisplayCacheOptions oneBit;
    oneBit.cropToFill = cropToFill;
    oneBit.mode = ImageRenderMode::OneBit;
    oneBit.renderPlane = static_cast<uint8_t>(GfxRenderer::BW);
    ImageDisplayCache::remove(renderer, imagePath, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), oneBit);

    ImageDisplayCacheOptions fastLsb;
    fastLsb.cropToFill = cropToFill;
    fastLsb.mode = ImageRenderMode::TwoBit;
    fastLsb.renderPlane = static_cast<uint8_t>(GfxRenderer::GRAYSCALE_LSB);
    ImageDisplayCache::remove(renderer, imagePath, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(),
                              fastLsb);

    ImageDisplayCacheOptions fastMsb = fastLsb;
    fastMsb.renderPlane = static_cast<uint8_t>(GfxRenderer::GRAYSCALE_MSB);
    ImageDisplayCache::remove(renderer, imagePath, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(),
                              fastMsb);

    ImageDisplayCacheOptions qualityLsb = fastLsb;
    qualityLsb.renderPlane = static_cast<uint8_t>(GfxRenderer::GRAY2_LSB);
    qualityLsb.quality = true;
    ImageDisplayCache::remove(renderer, imagePath, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(),
                              qualityLsb);

    ImageDisplayCacheOptions qualityMsb = qualityLsb;
    qualityMsb.renderPlane = static_cast<uint8_t>(GfxRenderer::GRAY2_MSB);
    ImageDisplayCache::remove(renderer, imagePath, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(),
                              qualityMsb);
  }
}

void removeCoverPath(GfxRenderer* renderer, const std::string& path) {
  if (path.empty()) {
    return;
  }
  if (renderer != nullptr) {
    purgeSleepImageDisplayCaches(*renderer, path);
  }
  if (SdMan.exists(path.c_str())) {
    SdMan.remove(path.c_str());
  }
}

uint32_t mixSleepImageSeed(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352dU;
  value ^= value >> 15;
  value *= 0x846ca68bU;
  value ^= value >> 16;
  return value;
}

uint32_t newSleepImageShuffleSeed() {
  uint32_t seed = esp_random() ^ micros() ^ (millis() << 16);
  if (seed == 0) {
    seed = 0x9e3779b9U;
  }
  return seed;
}

size_t gcdSize(size_t a, size_t b) {
  while (b != 0) {
    const size_t r = a % b;
    a = b;
    b = r;
  }
  return a;
}

size_t sleepImageStepForCount(const uint32_t seed, const size_t count) {
  if (count <= 1) {
    return 0;
  }

  size_t step = (mixSleepImageSeed(seed ^ 0x85ebca6bU) % (count - 1)) + 1;
  while (gcdSize(step, count) != 1) {
    step++;
    if (step >= count) {
      step = 1;
    }
  }
  return step;
}

size_t sleepImageIndexForPosition(const uint32_t seed, const uint32_t position, const size_t count) {
  if (count <= 1) {
    return 0;
  }

  const size_t offset = mixSleepImageSeed(seed ^ 0xc2b2ae35U) % count;
  const size_t step = sleepImageStepForCount(seed, count);
  return (offset + (static_cast<size_t>(position) % count) * step) % count;
}

void beginNewSleepImageCycleIfNeeded(const size_t count) {
  if (count <= 1) {
    if (APP_STATE.sleepImageShuffleSeed == 0) {
      APP_STATE.sleepImageShuffleSeed = newSleepImageShuffleSeed();
    }
    return;
  }

  if (APP_STATE.sleepImageShuffleSeed == 0) {
    APP_STATE.sleepImageShuffleSeed = newSleepImageShuffleSeed();
  }

  if (APP_STATE.lastSleepImage % count != 0) {
    return;
  }

  const uint32_t previousSeed = APP_STATE.sleepImageShuffleSeed;
  const size_t previousIndex = APP_STATE.lastSleepImage == 0
                                   ? count
                                   : sleepImageIndexForPosition(previousSeed, APP_STATE.lastSleepImage - 1, count);
  uint32_t nextSeed = previousSeed;

  for (int attempts = 0; attempts < 8; ++attempts) {
    nextSeed = newSleepImageShuffleSeed();
    const size_t nextIndex = sleepImageIndexForPosition(nextSeed, 0, count);
    if (previousIndex >= count || nextIndex != previousIndex) {
      break;
    }
  }

  APP_STATE.sleepImageShuffleSeed = nextSeed;
}

std::string pickSleepBmpPath(const bool ignoreFixed = false) {
  if (!ignoreFixed) {
    std::string fixed = pathForFixedSleepBmp();
    if (!fixed.empty()) {
      return fixed;
    }
  }

  const std::vector<std::string> sleepImages = listSleepImagePaths();
  if (!sleepImages.empty()) {
    beginNewSleepImageCycleIfNeeded(sleepImages.size());
    const size_t count = sleepImages.size();
    std::string selected =
        sleepImages[sleepImageIndexForPosition(APP_STATE.sleepImageShuffleSeed, APP_STATE.lastSleepImage, count)];
    if (ignoreFixed && count > 1 && selected == APP_STATE.lastSleepImagePath) {
      for (size_t offset = 1; offset < count; ++offset) {
        const std::string& alternate = sleepImages[sleepImageIndexForPosition(
            APP_STATE.sleepImageShuffleSeed, APP_STATE.lastSleepImage + offset, count)];
        if (alternate != APP_STATE.lastSleepImagePath) {
          selected = alternate;
          break;
        }
      }
    }
    return selected;
  }
  constexpr const char* fallbackPaths[] = {"/sleep.bmp", "/sleep.jpg", "/sleep.jpeg"};
  std::vector<std::string> fallbackImages;
  std::copy_if(std::begin(fallbackPaths), std::end(fallbackPaths), std::back_inserter(fallbackImages),
      [](const char* path) { return SdMan.exists(path) && isSleepImageShuffleEnabled(path); });
  if (!fallbackImages.empty()) {
    const size_t count = fallbackImages.size();
    beginNewSleepImageCycleIfNeeded(count);
    std::string selected = fallbackImages[sleepImageIndexForPosition(
        APP_STATE.sleepImageShuffleSeed, APP_STATE.lastSleepImage, count)];
    if (ignoreFixed && count > 1 && selected == APP_STATE.lastSleepImagePath) {
      for (size_t offset = 1; offset < count; ++offset) {
        const std::string& alternate = fallbackImages[sleepImageIndexForPosition(
            APP_STATE.sleepImageShuffleSeed, APP_STATE.lastSleepImage + offset, count)];
        if (alternate != APP_STATE.lastSleepImagePath) {
          selected = alternate;
          break;
        }
      }
    }
    return selected;
  }
  return "";
}

std::string resolveLastReadCoverPathForSleep(const std::string& path) {
  std::string coverPath;

  if (StringUtils::checkFileExtension(path, ".epub")) {
    Epub book(path, "/.metadata/epub");
    if (book.load()) {
      const bool cropped = false;
      const std::string coverJpegPath = book.getCoverJpegPath(cropped);
      const std::string coverBmpPath = book.getCoverBmpPath(cropped);
      coverPath = SdMan.exists(coverJpegPath.c_str()) ? coverJpegPath : coverBmpPath;
      if (!SdMan.exists(coverPath.c_str())) {
        book.generateCoverBmp(cropped);
        coverPath = SdMan.exists(coverJpegPath.c_str()) ? coverJpegPath : coverBmpPath;
      }
      if (!SdMan.exists(coverPath.c_str())) {
        coverPath.clear();
      }
    }
  }

  if (StringUtils::checkFileExtension(path, ".xtc") || StringUtils::checkFileExtension(path, ".xtch")) {
    Xtc book(path, "/.metadata/xtc");
    if (book.load()) {
      book.setupCacheDir();
      if (!SdMan.exists(book.getCoverBmpPath().c_str())) {
        book.generateCoverBmp();
      }
      if (SdMan.exists(book.getCoverBmpPath().c_str())) {
        coverPath = book.getCoverBmpPath();
      }
    }
  }

  if (StringUtils::checkFileExtension(path, ".txt")) {
    Txt book(path, "/.system");
    if (book.load() && book.generateCoverBmp()) {
      coverPath = book.getCoverBmpPath();
    }
  }

  return coverPath;
}
}  // namespace

bool SleepActivity::shouldScheduleImageRotation(const bool sleptFromReader) {
  if (!SETTINGS.sleepImageRotationEnabled) {
    return false;
  }
  const bool showingCustomImage = SETTINGS.sleepScreen == SystemSetting::SLEEP_SCREEN_MODE::CUSTOM ||
                                  SETTINGS.sleepScreen == SystemSetting::SLEEP_SCREEN_MODE::TRANSPARENT ||
                                  (SETTINGS.sleepScreen == SystemSetting::SLEEP_SCREEN_MODE::HYBRID &&
                                   !sleptFromReader);
  if (!showingCustomImage) {
    return false;
  }
  return true;
}

bool SleepActivity::shouldEnablePowerDoublePressImageAdvance(const bool sleptFromReader) {
  const bool showingCustomImage = SETTINGS.sleepScreen == SystemSetting::SLEEP_SCREEN_MODE::CUSTOM ||
                                  SETTINGS.sleepScreen == SystemSetting::SLEEP_SCREEN_MODE::TRANSPARENT ||
                                  (SETTINGS.sleepScreen == SystemSetting::SLEEP_SCREEN_MODE::HYBRID &&
                                   !sleptFromReader);
  return showingCustomImage && SETTINGS.sleepImagePowerDoublePress;
}

bool SleepActivity::regenerateLastReadCoverForSleep(GfxRenderer* renderer) {
  if (APP_STATE.lastRead.empty()) {
    return false;
  }

  const std::string& path = APP_STATE.lastRead;
  if (StringUtils::checkFileExtension(path, ".epub")) {
    Epub book(path, "/.metadata/epub");
    if (!book.load()) {
      return false;
    }
    book.setupCacheDir();
    for (const bool cropped : {false, true}) {
      removeCoverPath(renderer, book.getCoverJpegPath(cropped));
      removeCoverPath(renderer, book.getCoverBmpPath(cropped));
    }
    const bool generated = book.generateCoverBmp(/*cropped=*/false);
    return generated && (SdMan.exists(book.getCoverJpegPath(false).c_str()) ||
                         SdMan.exists(book.getCoverBmpPath(false).c_str()));
  }

  if (StringUtils::checkFileExtension(path, ".xtc") || StringUtils::checkFileExtension(path, ".xtch")) {
    Xtc book(path, "/.metadata/xtc");
    if (!book.load()) {
      return false;
    }
    book.setupCacheDir();
    removeCoverPath(renderer, book.getCoverBmpPath());
    return book.generateCoverBmp() && SdMan.exists(book.getCoverBmpPath().c_str());
  }

  if (StringUtils::checkFileExtension(path, ".txt")) {
    Txt book(path, "/.system");
    if (!book.load()) {
      return false;
    }
    removeCoverPath(renderer, book.getCoverBmpPath());
    return book.generateCoverBmp() && SdMan.exists(book.getCoverBmpPath().c_str());
  }

  return false;
}

/**
 * @brief Initializes and renders the sleep screen when activity becomes active.
 *
 * Selects and renders the appropriate sleep screen based on the current
 * sleep screen mode setting (transparent, blank, custom, cover, or default).
 */
void SleepActivity::onEnter() {
  Activity::onEnter();
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 100);

  // HIGH-quality cover/custom images: our quality LUT doesn't fully clear to white on its own (whites come out
  // gray without a white baseline), so we pre-clear to white with a HALF refresh. A FAST pre-clear only fully
  // whitens a mostly-white incoming screen (e.g. a book text page); coming from the home/library screen (dark
  // covers) a single FAST pass leaves gray residue and the quality refresh then starts dirty. HALF reliably
  // clears from any screen.
  const bool renderDateTime =
      SETTINGS.sleepScreen == SystemSetting::SLEEP_SCREEN_MODE::DATETIME && dateTimeSleepScreenAvailable();
  if (SETTINGS.sleepScreen != SystemSetting::SLEEP_SCREEN_MODE::TRANSPARENT && !renderDateTime) {
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 101);
    renderer.clearScreen(0Xff);
    renderer.displayBuffer();
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 102);
  }

  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 110,
                                static_cast<uint32_t>(SETTINGS.sleepScreen));
  switch (SETTINGS.sleepScreen) {
    case SystemSetting::SLEEP_SCREEN_MODE::TRANSPARENT:
      sleepImageRenderSucceeded = renderTransparentSleepScreen();
      break;
    case SystemSetting::SLEEP_SCREEN_MODE::BLANK:
      renderBlankSleepScreen();
      break;
    case SystemSetting::SLEEP_SCREEN_MODE::CUSTOM:
      sleepImageRenderSucceeded = renderCustomSleepScreen();
      break;
    case SystemSetting::SLEEP_SCREEN_MODE::COVER:
      renderCoverSleepScreen();
      break;
    case SystemSetting::SLEEP_SCREEN_MODE::HYBRID:
      if (sleptFromReader) {
        renderCoverSleepScreen();
      } else {
        sleepImageRenderSucceeded = renderCustomSleepScreen();
      }
      break;
    case SystemSetting::SLEEP_SCREEN_MODE::DATETIME:
      if (dateTimeSleepScreenAvailable()) {
        renderDateTimeSleepScreen();
      } else {
        renderDefaultSleepScreen();
      }
      break;
    default:
      renderDefaultSleepScreen();
      break;
  }
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 111,
                                sleepImageRenderSucceeded ? 1UL : 0UL);
}

/**
 * @brief Renders a custom sleep screen from user-provided images.
 *
 * Uses a fixed image from settings when set; otherwise picks randomly from /sleep/
 * and SD-root sleep.bmp/jpg/jpeg. Falls back to default sleep screen if no images are found.
 */
bool SleepActivity::renderCustomSleepScreen() const {
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 200);
  const std::string imagePath = pickSleepBmpPath(forceSleepImageAdvance);
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 201,
                                static_cast<uint32_t>(imagePath.size()));
  if (forceSleepImageAdvance && !imagePath.empty() && imagePath == APP_STATE.lastSleepImagePath) {
    renderSleepImageDiagnostic(renderer, "No different sleep image");
    return false;
  }
  if (!imagePath.empty()) {
    SleepImageToneGuard toneGuard(renderer);

    if (isSleepImagePathJpeg(imagePath)) {
      HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 202);
      ImageRender::Options options = sleepImageOptions();
      const bool willDisplayGrayscale = sleepTwoBitEnabled();
      const bool rendered = ImageRender::create(renderer, imagePath)
                                .render(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), options);
      HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 203, rendered ? 1UL : 0UL);
      if (rendered) {
        if (!willDisplayGrayscale || !sleepImageQualityEnabled()) {
          renderer.displayBuffer();
        }
        HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 204);
        const bool grayscaleRendered = runSleepImageTwoBitPasses(renderer, imagePath, options);
        HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 205,
                                      grayscaleRendered ? 1UL : 0UL);
        if (grayscaleRendered) {
          recordSleepImageUsed(imagePath);
          return true;
        }
        purgeSleepImageDisplayCaches(renderer, imagePath);
        if (!sleepImageQualityEnabled()) {
          recordSleepImageUsed(imagePath);
          return true;
        }
      }
    }
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 206);
    FsFile file;
    const bool opened = SdMan.openFileForRead("SLP", imagePath, file);
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 207, opened ? 1UL : 0UL);
    if (opened) {
      Bitmap bitmap(file);
      const BmpReaderError headerResult = bitmap.parseHeaders();
      HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 208,
                                    headerResult == BmpReaderError::Ok ? 1UL : 0UL);
      if (headerResult == BmpReaderError::Ok) {
        if (SETTINGS.sleepScreenCoverMode == SystemSetting::SLEEP_SCREEN_COVER_MODE::FIT) {
          renderFill(bitmap);
        } else {
          renderBitmapSleepScreen(bitmap);
        }
        HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 209);
        file.close();
        recordSleepImageUsed(imagePath);
        return true;
      }
      file.close();
    }
    if (forceSleepImageAdvance) {
      renderSleepImageDiagnostic(renderer, "Sleep image render failed");
      return false;
    }
  }

  if (forceSleepImageAdvance) {
    renderSleepImageDiagnostic(renderer, "No eligible sleep image");
    return false;
  }
  renderDefaultSleepScreen();
  return false;
}

/**
 * @brief Renders a transparent overlay sleep screen.
 *
 * Displays a semi-transparent image overlay on top of the current screen content.
 */
bool SleepActivity::renderTransparentSleepScreen() const {
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 230);
  const std::string imagePath = pickSleepBmpPath(forceSleepImageAdvance);
  HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 231,
                                static_cast<uint32_t>(imagePath.size()));
  if (forceSleepImageAdvance && !imagePath.empty() && imagePath == APP_STATE.lastSleepImagePath) {
    renderSleepImageDiagnostic(renderer, "No different sleep image");
    return false;
  }
  if (!imagePath.empty()) {
    SleepImageToneGuard toneGuard(renderer);
    // Transparent overlays only work at LOW/MEDIUM. At HIGH the quality LUT can't composite over the existing
    // screen, so remove the background (clear to white) and render the image opaque instead.
    const bool removeBackground = sleepImageQualityEnabled();
    if (isSleepImagePathJpeg(imagePath)) {
      HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 232);
      ImageRender::Options options = sleepImageOptions(/*allowQuality=*/false);
      options.useDisplayCache = removeBackground;
      if (removeBackground) {
        renderer.clearScreen();
      }
      const bool rendered = ImageRender::create(renderer, imagePath)
                                .render(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), options);
      HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 233, rendered ? 1UL : 0UL);
      if (rendered) {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 234);
        if (runSleepImageTwoBitPasses(renderer, imagePath, options, /*allowQuality=*/false)) {
          HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 235, 1);
          recordSleepImageUsed(imagePath);
          return true;
        }
        HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 235, 0);
        purgeSleepImageDisplayCaches(renderer, imagePath);
        recordSleepImageUsed(imagePath);
        return true;
      }
    }
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 236);
    FsFile file;
    const bool opened = SdMan.openFileForRead("SLP", imagePath, file);
    HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 237, opened ? 1UL : 0UL);
    if (opened) {
      Bitmap bitmap(file);
      const BmpReaderError headerResult = bitmap.parseHeaders();
      HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 238,
                                    headerResult == BmpReaderError::Ok ? 1UL : 0UL);
      if (headerResult == BmpReaderError::Ok) {
        if (removeBackground) {
          renderer.clearScreen();
        }
        renderer.bitmap.transparent(bitmap, 0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), 1);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        HalGPIO::recordSleepWakeTrace(HalGPIO::SleepWakeTraceEvent::SleepStage, 239);
        file.close();
        recordSleepImageUsed(imagePath);
        return true;
      }
      file.close();
    }
    if (forceSleepImageAdvance) {
      renderSleepImageDiagnostic(renderer, "Sleep image render failed");
      return false;
    }
  }

  if (forceSleepImageAdvance) {
    renderSleepImageDiagnostic(renderer, "No eligible sleep image");
    return false;
  }
  renderDefaultSleepScreen();
  return false;
}

/**
 * @brief Renders the cover of the last opened book as sleep screen.
 *
 * Extracts and displays the cover image from the most recently opened book
 * (EPUB, XTC, or TXT format). Applies cropping or scaling based on settings.
 */
void SleepActivity::renderCoverSleepScreen() const {
  if (APP_STATE.lastRead.empty()) {
    renderCustomSleepScreen();
    return;
  }

  const std::string coverPath = resolveLastReadCoverPathForSleep(APP_STATE.lastRead);

  if (!coverPath.empty() && isSleepImagePathJpeg(coverPath)) {
    SleepImageToneGuard toneGuard(renderer);
    renderer.clearScreen();
    ImageRender::Options options = sleepImageOptions();
    const bool willDisplayGrayscale = sleepTwoBitEnabled();
    if (ImageRender::create(renderer, coverPath)
            .render(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight(), options)) {
      if (SETTINGS.sleepScreenCoverFilter == SystemSetting::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
        renderer.invertScreen();
      }
      if (!willDisplayGrayscale || !sleepImageQualityEnabled()) {
        renderer.displayBuffer();
      }
      if (runSleepImageTwoBitPasses(renderer, coverPath, options)) {
        return;
      }
      purgeSleepImageDisplayCaches(renderer, coverPath);
      if (!sleepImageQualityEnabled()) {
        return;
      }
    }
    removeCoverPath(&renderer, coverPath);
    regenerateLastReadCoverForSleep(&renderer);
    renderCustomSleepScreen();
    return;
  }

  FsFile file;
  if (!coverPath.empty() && SdMan.openFileForRead("SLP", coverPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      SleepImageToneGuard toneGuard(renderer);
      if (SETTINGS.sleepScreenCoverMode == SystemSetting::SLEEP_SCREEN_COVER_MODE::FIT) {
        renderFill(bitmap);
      } else {
        renderBitmapSleepScreen(bitmap);
      }
    }
    file.close();
    return;
  }

  renderCustomSleepScreen();
}

void SleepActivity::renderFill(const Bitmap& bitmap) const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  float cropX = 0.0f;
  float cropY = 0.0f;
  constexpr int x = 0;
  constexpr int y = 0;

  const float iw = static_cast<float>(bitmap.getWidth());
  const float ih = static_cast<float>(bitmap.getHeight());
  if (iw > 0.f && ih > 0.f) {
    const float ir = iw / ih;
    const float tr = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
    if (ir > tr) {
      cropX = 1.0f - (tr / ir);
    } else if (ir < tr) {
      cropY = 1.0f - (ir / tr);
    }
  }

  renderer.clearScreen();

  const bool hasTwoBit = bitmap.hasGreyscale() && sleepTwoBitEnabled();

  // Use drawSleepScreen (not drawBitmap) so 2-bit off matches Crop mode: sleep uses a
  // stricter BW map; drawBitmap would still dither 2bpp and look grey when Fill is selected.
  constexpr bool kCoverFill = true;
  renderer.bitmap.sleepScreen(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, kCoverFill, sleepImageRenderMode());

  if (SETTINGS.sleepScreenCoverFilter == SystemSetting::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // Quality grayscale does a single clean+draw refresh; skip the BW pre-flash so it's one flash, not two.
  if (!(hasTwoBit && sleepImageQualityEnabled())) {
    renderer.displayBuffer();
  }

  if (hasTwoBit) {
    const bool quality = sleepImageQualityEnabled();
    if (quality) {
      renderer.renderGrayscalePasses(true, false, [&] {
        bitmap.rewindToData();
        renderer.clearScreen(0xFF);
        renderer.bitmap.sleepScreen(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, kCoverFill,
                                    ImageRenderMode::TwoBit);
      });
    } else {
      bitmap.rewindToData();
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderer.bitmap.sleepScreen(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, kCoverFill,
                                  ImageRenderMode::TwoBit);
      renderer.copyGrayscaleLsbBuffers();

      bitmap.rewindToData();
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderer.bitmap.sleepScreen(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, kCoverFill,
                                  ImageRenderMode::TwoBit);
      renderer.copyGrayscaleMsbBuffers();

      renderer.displayGrayBuffer(false);
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.cleanupGrayscaleWithFrameBuffer();
    }
  }
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool preCroppedEpubCover) const {
  (void)preCroppedEpubCover;

  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  Serial.printf("[SLP] bitmap %d x %d, screen %d x %d\n", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    Serial.printf("[SLP] bitmap ratio: %f, screen ratio: %f\n", ratio, screenRatio);
    if (ratio > screenRatio) {
      if (SETTINGS.sleepScreenCoverMode == SystemSetting::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        Serial.printf("[SLP] Cropping bitmap x: %f\n", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = static_cast<int>(std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2));
      Serial.printf("[SLP] Centering with ratio %f to y=%d\n", ratio, y);
    } else {
      if (SETTINGS.sleepScreenCoverMode == SystemSetting::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        Serial.printf("[SLP] Cropping bitmap y: %f\n", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = static_cast<int>(std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2));
      y = 0;
      Serial.printf("[SLP] Centering with ratio %f to x=%d\n", ratio, x);
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  Serial.printf("[SLP] drawing to %d x %d\n", x, y);
  renderer.clearScreen();

  const bool coverFill = SETTINGS.sleepScreenCoverMode == SystemSetting::SLEEP_SCREEN_COVER_MODE::FIT;
  const bool hasTwoBit = bitmap.hasGreyscale() && sleepTwoBitEnabled();

  renderer.bitmap.sleepScreen(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, coverFill, sleepImageRenderMode());

  if (SETTINGS.sleepScreenCoverFilter == SystemSetting::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // Quality grayscale does a single clean+draw refresh; skip the BW pre-flash so it's one flash, not two.
  if (!(hasTwoBit && sleepImageQualityEnabled())) {
    renderer.displayBuffer();
  }

  if (hasTwoBit) {
    const bool quality = sleepImageQualityEnabled();
    if (quality) {
      renderer.renderGrayscalePasses(true, false, [&] {
        bitmap.rewindToData();
        renderer.clearScreen(0xFF);
        renderer.bitmap.sleepScreen(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, coverFill,
                                    ImageRenderMode::TwoBit);
      });
    } else {
      bitmap.rewindToData();
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderer.bitmap.sleepScreen(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, coverFill,
                                  ImageRenderMode::TwoBit);
      renderer.copyGrayscaleLsbBuffers();

      bitmap.rewindToData();
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderer.bitmap.sleepScreen(bitmap, x, y, pageWidth, pageHeight, cropX, cropY, coverFill,
                                  ImageRenderMode::TwoBit);
      renderer.copyGrayscaleMsbBuffers();

      renderer.displayGrayBuffer(false);
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.cleanupGrayscaleWithFrameBuffer();
    }
  }
}

/**
 * @brief Renders the default sleep screen with Corgi logo.
 *
 * Displays the CorgiSleep logo centered on the screen with optional inversion
 * based on sleep screen mode settings.
 */
void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.rectangle.fill(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight());
  renderer.clearScreen();
  renderer.bitmap.icon(CorgiSleep, (pageWidth - 256) / 2, (pageHeight - 256) / 2, 256, 256);

  if (SETTINGS.sleepScreen != SystemSetting::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

/**
 * @brief Renders a minimal date/time sleep screen using the X3 RTC.
 */
void SleepActivity::renderDateTimeSleepScreen() const {
  bool hasClock = false;
  SleepClockRenderer::DateTimeView view;
#ifndef SIMULATOR
  HalGPIO::DateTime dateTime;
  if (gpio.deviceIsX3()) {
    hasClock = gpio.readDateTime(dateTime);
    if (hasClock) {
      view.year = dateTime.year;
      view.month = dateTime.month;
      view.day = dateTime.day;
      view.hour = dateTime.hour;
      view.minute = dateTime.minute;
      view.weekday = dateTime.weekday;
    }
  }
#endif

  renderer.clearScreen(0xff);
  SleepClockRenderer::render(renderer, SETTINGS.sleepClockStyle, view, hasClock, 0, 0, renderer.getScreenWidth(),
                             renderer.getScreenHeight());

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

/**
 * @brief Renders a completely blank sleep screen.
 *
 * Clears the screen to save power and prevent screen burn-in during sleep.
 */
void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
