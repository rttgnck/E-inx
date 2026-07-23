/**
 * @file EditMetadataActivity.cpp
 * @brief Definitions for EditMetadataActivity.
 */

#include "EditMetadataActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>

#include <functional>

#include "../util/KeyboardEntryActivity.h"
#include "state/BookState.h"
#include "state/RecentBooks.h"
#include "state/Statistics.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "util/StringUtils.h"

namespace {
bool isXtcBookPath(const std::string& bookPath) {
  return StringUtils::checkFileExtension(bookPath, ".xtc") || StringUtils::checkFileExtension(bookPath, ".xtch");
}

bool isEpubBookPath(const std::string& bookPath) { return StringUtils::checkFileExtension(bookPath, ".epub"); }

bool isTxtBookPath(const std::string& bookPath) {
  return StringUtils::checkFileExtension(bookPath, ".txt") || StringUtils::checkFileExtension(bookPath, ".md");
}

std::string hashedBookPath(const std::string& bookPath) {
  return std::to_string(std::hash<std::string>{}(bookPath));
}

std::string statsCachePathForBookPath(const std::string& bookPath) {
  const char* root = isXtcBookPath(bookPath) ? "/.metadata/xtc" : "/.metadata/epub";
  return std::string(root) + "/" + hashedBookPath(bookPath);
}

std::string bookCachePathForBookPath(const std::string& bookPath) {
  if (isXtcBookPath(bookPath)) {
    return std::string("/.metadata/xtc/") + hashedBookPath(bookPath);
  }
  if (isTxtBookPath(bookPath)) {
    return std::string("/.system/txt_") + hashedBookPath(bookPath);
  }
  return std::string("/.metadata/epub/") + hashedBookPath(bookPath);
}

bool ensureDirectoryPath(const std::string& path) {
  if (path.empty() || path[0] != '/') {
    return false;
  }

  size_t pos = 1;
  while (pos <= path.length()) {
    const size_t next = path.find('/', pos);
    const std::string segment = path.substr(0, next == std::string::npos ? path.length() : next);
    if (!segment.empty() && !SdMan.exists(segment.c_str())) {
      SdMan.mkdir(segment.c_str());
    }
    if (next == std::string::npos) {
      break;
    }
    pos = next + 1;
  }
  return SdMan.exists(path.c_str());
}

constexpr int kRowCount = 6;
}  // namespace

void EditMetadataActivity::onEnter() {
  Activity::onEnter();
  statusMessage.clear();
  render();
}

void EditMetadataActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    selected = (selected - 1 + kRowCount) % kRowCount;
    statusMessage.clear();
    render();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    selected = (selected + 1) % kRowCount;
    statusMessage.clear();
    render();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
  }
}

void EditMetadataActivity::activateSelected() {
  switch (selected) {
    case 0:
      enterNewActivity(new KeyboardEntryActivity(
          renderer, mappedInput, "Title", title, 10, 200, false,
          [this](const std::string& value) {
            title = value;
            exitActivity();
            render();
          },
          [this]() {
            exitActivity();
            render();
          }));
      break;
    case 1:
      enterNewActivity(new KeyboardEntryActivity(
          renderer, mappedInput, "Author", author, 10, 200, false,
          [this](const std::string& value) {
            author = value;
            exitActivity();
            render();
          },
          [this]() {
            exitActivity();
            render();
          }));
      break;
    case 2:
      statusMessage = "Language is read only";
      render();
      break;
    case 3:
      favorite = !favorite;
      statusMessage.clear();
      render();
      break;
    case 4:
      clearBookCache();
      break;
    case 5:
      save();
      break;
    default:
      break;
  }
}

void EditMetadataActivity::clearBookCache() {
  const std::string cachePath = bookCachePathForBookPath(bookPath);
  const std::string statsCachePath = statsCachePathForBookPath(bookPath);
  const bool canPreserveBookStats = isEpubBookPath(bookPath) || isXtcBookPath(bookPath);

  BookReadingStats preservedStats;
  const bool hadStats = canPreserveBookStats && loadBookStats(statsCachePath.c_str(), preservedStats);

  std::string overrideTitle;
  std::string overrideAuthor;
  const bool hadMetadataOverride =
      isEpubBookPath(bookPath) && Epub::readMetadataOverride(statsCachePath, overrideTitle, overrideAuthor);

  if (!SdMan.exists(cachePath.c_str())) {
    statusMessage = hadStats ? "No cache found (stats kept)" : "No cache found";
    render();
    return;
  }

  Serial.printf("[%lu] [EDIT_META] Clearing cache for %s at %s\n", millis(), bookPath.c_str(), cachePath.c_str());
  if (!SdMan.removeDir(cachePath.c_str())) {
    statusMessage = "Cache clear failed";
    render();
    return;
  }

  int restoredItems = 0;
  if (hadStats) {
    ensureDirectoryPath(statsCachePath);
    preservedStats.path = statsCachePath;
    saveBookStats(statsCachePath.c_str(), preservedStats);
    restoredItems++;
  }
  if (hadMetadataOverride) {
    ensureDirectoryPath(statsCachePath);
    if (Epub::writeMetadataOverride(statsCachePath, overrideTitle, overrideAuthor)) {
      restoredItems++;
    }
  }

  if (restoredItems > 0) {
    statusMessage = "Cache cleared (stats kept)";
  } else {
    statusMessage = "Cache cleared";
  }
  render();
}

void EditMetadataActivity::save() {
  const std::string statsCachePath = statsCachePathForBookPath(bookPath);

  // 1) Override applied by Epub on load (reader / cover / stats screen title).
  if (StringUtils::checkFileExtension(bookPath, ".epub")) {
    Epub::writeMetadataOverride(statsCachePath, title, author);
  }

  // 2) Denormalized copies shown in the Library browser.
  BookState& books = BookState::getInstance();
  books.addOrUpdateBook(bookPath, title, author);
  if (BookState::Book* b = books.findBookByPath(bookPath)) {
    b->title = title;
    b->author = author;
    b->isFavorite = favorite;
  }
  books.saveToFile();

  // 3) Denormalized copies shown on the Recent (home) screen.
  RecentBooks::getInstance().updateMetadata(bookPath, title, author);

  // 4) Denormalized copies stored in per-book statistics, shown on the Statistics tab.
  BookReadingStats stats;
  if (loadBookStats(statsCachePath.c_str(), stats)) {
    stats.path = statsCachePath;
    stats.title = title;
    stats.author = author;
    saveBookStats(statsCachePath.c_str(), stats);
  }

  statusMessage = "Saved";
  render();
  onBack();
}

void EditMetadataActivity::render() {
  renderer.clearScreen();
  const int screenW = renderer.getScreenWidth();
  const int titleFont = ATKINSON_HYPERLEGIBLE_12_FONT_ID;
  const int rowFont = ATKINSON_HYPERLEGIBLE_10_FONT_ID;

  renderer.text.render(titleFont, 24, 24, "Edit Metadata", true, EpdFontFamily::BOLD);
  renderer.line.render(0, 58, screenW, 58, true);

  const char* labels[kRowCount] = {"Title", "Author", "Language", "Favorite", "Clear Cache", "Save"};
  const int rowH = 54;
  const int firstY = 72;
  const int rowLh = renderer.text.getLineHeight(rowFont);

  for (int i = 0; i < kRowCount; i++) {
    const int rowY = firstY + i * rowH;
    const bool sel = (i == selected);
    if (sel) {
      renderer.rectangle.fill(12, rowY, screenW - 24, rowH - 8, true);
    }
    const int textY = rowY + (rowH - 8 - rowLh) / 2;
    renderer.text.render(rowFont, 28, textY, labels[i], !sel, EpdFontFamily::BOLD);

    std::string value;
    if (i == 0) {
      value = title.empty() ? "(none)" : title;
    } else if (i == 1) {
      value = author.empty() ? "(none)" : author;
    } else if (i == 2) {
      value = language.empty() ? "(none)" : language;
    } else if (i == 3) {
      value = favorite ? "On" : "Off";
    }
    if (!value.empty()) {
      const int maxW = screenW - 28 - 120;
      const std::string shown = renderer.text.truncate(rowFont, value.c_str(), maxW);
      const int vw = renderer.text.getWidth(rowFont, shown.c_str());
      renderer.text.render(rowFont, screenW - 28 - vw, textY, shown.c_str(), !sel);
    }
  }

  if (!statusMessage.empty()) {
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, firstY + kRowCount * rowH + 20, statusMessage.c_str(), true);
  }

  const char* confirmLabel = "Edit";
  if (selected == 2) {
    confirmLabel = "Info";
  } else if (selected == 3) {
    confirmLabel = "Toggle";
  } else if (selected == 4) {
    confirmLabel = "Clear";
  } else if (selected == 5) {
    confirmLabel = "Save";
  }
  const auto lbls = mappedInput.mapLabels("\xC2\xAB Back", confirmLabel, "\xE2\x96\xB2", "\xE2\x96\xBC");
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, lbls.btn1, lbls.btn2, lbls.btn3, lbls.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
