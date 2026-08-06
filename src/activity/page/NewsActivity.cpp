#include "NewsActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <SDCardManager.h>

#include <algorithm>
#include <ctime>

#include "network/HttpDownloader.h"
#include "state/NetworkCredential.h"
#include "state/SystemSetting.h"
#include "system/Fonts.h"
#include "system/MenuNav.h"

extern HalGPIO gpio;

#ifndef SIMULATOR
#include <WiFi.h>
#endif

namespace {
constexpr int LIST_ITEM_HEIGHT = 60;
constexpr int HEADER_HEIGHT = 50;
constexpr int DOWNLOAD_BTN_INDEX = -1;
}  // namespace

void NewsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = DOWNLOAD_BTN_INDEX;
  scrollOffset = 0;
  actionMenuOpen = false;
  downloadState = DownloadState::IDLE;

  if (!SdMan.exists(NEWS_DIR)) {
    SdMan.mkdir(NEWS_DIR);
  }
  if (!SdMan.exists(ARCHIVE_DIR)) {
    SdMan.mkdir(ARCHIVE_DIR);
  }
  if (!SdMan.exists(BOOKMARK_DIR)) {
    SdMan.mkdir(BOOKMARK_DIR);
  }

  scanNewsFolder();
  render();
  SETTINGS.runHalfRefreshOnLoadIfEnabled(renderer, SystemSetting::RefreshOnLoadPage::Sync);
}

void NewsActivity::onExit() { Activity::onExit(); }

std::string NewsActivity::todayFilename() {
  char buf[16];
#ifndef SIMULATOR
  HalGPIO::DateTime dt;
  if (gpio.readDateTime(dt) && dt.year >= 2024) {
    snprintf(buf, sizeof(buf), "%02u-%02u-%04u.epub", dt.month, dt.day, dt.year);
    return std::string(buf);
  }
#endif
  time_t now;
  time(&now);
  struct tm ti;
  localtime_r(&now, &ti);
  snprintf(buf, sizeof(buf), "%02d-%02d-%04d.epub", ti.tm_mon + 1, ti.tm_mday, ti.tm_year + 1900);
  return std::string(buf);
}

std::string NewsActivity::buildDownloadUrl() {
  std::string baseUrl = SETTINGS.newsRepoUrl;
  if (baseUrl.empty()) {
    baseUrl = "https://raw.githubusercontent.com/rttgnck/news-reader/main/archive";
  }
  if (baseUrl.back() == '/') {
    baseUrl.pop_back();
  }
  return baseUrl + "/" + todayFilename();
}

void NewsActivity::scanNewsFolder() {
  entries.clear();

  auto addFromDir = [this](const char* dir, bool isBookmarked, bool isArchived) {
    if (!SdMan.exists(dir)) return;
    std::vector<String> files = SdMan.listFiles(dir);
    for (const auto& f : files) {
      std::string fname = f.c_str();
      if (fname.length() < 5) continue;
      std::string ext = fname.substr(fname.length() - 5);
      if (ext != ".epub") continue;

      NewsEntry entry;
      entry.filename = fname;
      entry.bookmarked = isBookmarked;
      entry.archived = isArchived;

      std::string datePart = fname.substr(0, fname.length() - 5);
      entry.displayDate = datePart;

      entry.path = std::string(dir) + "/" + fname;
      entries.push_back(entry);
    }
  };

  addFromDir(BOOKMARK_DIR, true, false);
  addFromDir(NEWS_DIR, false, false);

  std::sort(entries.begin(), entries.end(), [](const NewsEntry& a, const NewsEntry& b) {
    if (a.bookmarked != b.bookmarked) return a.bookmarked;
    return a.filename > b.filename;
  });
}

int NewsActivity::getVisibleItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const int availableHeight = screenHeight - TAB_BAR_HEIGHT - HEADER_HEIGHT - 80;
  return std::max(1, availableHeight / LIST_ITEM_HEIGHT);
}

void NewsActivity::loop() {
  if (tabSelectorIndex == 1 && updateRequired) {
    updateRequired = false;
    render();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      SETTINGS.shortPwrBtn == SystemSetting::SHORT_PWRBTN::PAGE_REFRESH) {
    renderer.displayBuffer(HalDisplay::MANUAL_REFRESH);
    updateRequired = true;
    return;
  }

  if (downloadState == DownloadState::CONNECTING || downloadState == DownloadState::DOWNLOADING) {
    return;
  }

  if (downloadState == DownloadState::SUCCESS || downloadState == DownloadState::ERROR ||
      downloadState == DownloadState::ALREADY_EXISTS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      downloadState = DownloadState::IDLE;
      scanNewsFolder();
      updateRequired = true;
    }
    return;
  }

  const bool confirmPressed = mappedInput.wasPressed(MappedInputManager::Button::Confirm);
  const bool upPressed = mappedInput.wasPressed(MenuNav::itemPrev());
  const bool downPressed = mappedInput.wasPressed(MenuNav::itemNext());
  const bool backPressed = mappedInput.wasPressed(MappedInputManager::Button::Back);

  if (backPressed) {
    if (actionMenuOpen) {
      actionMenuOpen = false;
      updateRequired = true;
      return;
    }
    if (mappedInput.getHeldTime() >= 300 && onRecentOpen) {
      vTaskDelay(pdMS_TO_TICKS(300));
      onRecentOpen();
    }
    return;
  }

  if (mappedInput.wasPressed(MenuNav::tabPrev())) {
    tabSelectorIndex = 0;
    navigateToSelectedMenu();
    return;
  }

  if (mappedInput.wasPressed(MenuNav::tabNext())) {
    tabSelectorIndex = 2;
    navigateToSelectedMenu();
    return;
  }

  if (tabSelectorIndex != 1) {
    return;
  }

  if (actionMenuOpen) {
    if (upPressed) {
      actionMenuIndex = (actionMenuIndex + 2) % 3;
      updateRequired = true;
    }
    if (downPressed) {
      actionMenuIndex = (actionMenuIndex + 1) % 3;
      updateRequired = true;
    }
    if (confirmPressed) {
      if (selectedIndex >= 0 && selectedIndex < static_cast<int>(entries.size())) {
        switch (actionMenuIndex) {
          case 0:
            deleteEntry(selectedIndex);
            break;
          case 1:
            if (entries[selectedIndex].bookmarked) {
              unbookmarkEntry(selectedIndex);
            } else {
              bookmarkEntry(selectedIndex);
            }
            break;
          case 2:
            archiveEntry(selectedIndex);
            break;
        }
      }
      actionMenuOpen = false;
      scanNewsFolder();
      if (selectedIndex >= static_cast<int>(entries.size())) {
        selectedIndex = entries.empty() ? DOWNLOAD_BTN_INDEX : static_cast<int>(entries.size()) - 1;
      }
      updateRequired = true;
    }
    return;
  }

  const int totalItems = static_cast<int>(entries.size());
  const int visibleItems = getVisibleItems();

  if (confirmPressed) {
    if (selectedIndex == DOWNLOAD_BTN_INDEX) {
      startDownload();
      return;
    }
    if (selectedIndex >= 0 && selectedIndex < totalItems) {
      onOpenBook(entries[selectedIndex].path);
      return;
    }
  }

  if (upPressed) {
    if (selectedIndex == DOWNLOAD_BTN_INDEX) {
      // already at top
    } else if (selectedIndex == 0) {
      selectedIndex = DOWNLOAD_BTN_INDEX;
    } else {
      selectedIndex--;
    }
    if (selectedIndex >= 0 && selectedIndex < scrollOffset) {
      scrollOffset = selectedIndex;
    }
    updateRequired = true;
  }

  if (downPressed) {
    if (selectedIndex == DOWNLOAD_BTN_INDEX) {
      if (!entries.empty()) {
        selectedIndex = 0;
      }
    } else if (selectedIndex < totalItems - 1) {
      selectedIndex++;
    }
    if (selectedIndex >= 0 && selectedIndex >= scrollOffset + visibleItems) {
      scrollOffset = selectedIndex - visibleItems + 1;
    }
    updateRequired = true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < 300) {
    if (selectedIndex >= 0 && selectedIndex < totalItems) {
      actionMenuOpen = true;
      actionMenuIndex = 0;
      updateRequired = true;
    }
  }
}

void NewsActivity::render() const {
  renderer.clearScreen();
  const int screenWidth = renderer.getScreenWidth();

  renderTabBar(renderer);

  const int headerY = TAB_BAR_HEIGHT;
  const int headerTextY = headerY + (HEADER_HEIGHT - renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_12_FONT_ID)) / 2;
  renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, 20, headerTextY, "Daily News", true, EpdFontFamily::BOLD);

  const int dividerY = headerY + HEADER_HEIGHT;
  renderer.line.render(0, dividerY, screenWidth, dividerY);

  if (downloadState != DownloadState::IDLE) {
    renderDownloadStatus();
  } else if (actionMenuOpen) {
    renderActionMenu();
  } else {
    renderList();
  }

  const char* backLabel = selectedIndex >= 0 ? "Actions" : "« Recent";
  const auto labels = mappedInput.mapLabels(backLabel, "Select", "", "");
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void NewsActivity::renderList() const {
  const int screenWidth = renderer.getScreenWidth();
  const int listStartY = TAB_BAR_HEIGHT + HEADER_HEIGHT;
  const int visibleItems = getVisibleItems();

  const bool downloadSelected = (selectedIndex == DOWNLOAD_BTN_INDEX);
  const int btnY = listStartY + 5;
  const int btnH = LIST_ITEM_HEIGHT - 10;

  if (downloadSelected) {
    renderer.rectangle.fill(10, btnY, screenWidth - 20, btnH, static_cast<int>(GfxRenderer::FillTone::Ink));
  } else {
    renderer.rectangle.render(10, btnY, screenWidth - 20, btnH);
  }

  const int btnTextY = btnY + (btnH - renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID)) / 2;
  const char* btnText = "Download Today's News";
  const int btnTextW = renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_10_FONT_ID, btnText);
  renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, (screenWidth - btnTextW) / 2, btnTextY, btnText,
                       !downloadSelected);

  const int itemStartY = listStartY + LIST_ITEM_HEIGHT + 5;
  renderer.line.render(0, itemStartY - 1, screenWidth, itemStartY - 1);

  if (entries.empty()) {
    const int emptyY = itemStartY + 30;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 20, emptyY, "No news yet. Download to get started.");
    return;
  }

  for (int i = 0; i < visibleItems && (i + scrollOffset) < static_cast<int>(entries.size()); i++) {
    const int entryIndex = i + scrollOffset;
    const auto& entry = entries[entryIndex];
    const int itemY = itemStartY + i * LIST_ITEM_HEIGHT;
    const bool isSelected = (entryIndex == selectedIndex);

    if (isSelected) {
      renderer.rectangle.fill(0, itemY, screenWidth, LIST_ITEM_HEIGHT, static_cast<int>(GfxRenderer::FillTone::Ink));
    }

    const int textY = itemY + (LIST_ITEM_HEIGHT - renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID)) / 2;

    std::string label;
    if (entry.bookmarked) {
      label = "* ";
    }
    label += entry.displayDate;

    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 20, textY, label.c_str(), !isSelected);

    if (i < visibleItems - 1 && (entryIndex + 1) < static_cast<int>(entries.size())) {
      renderer.line.render(0, itemY + LIST_ITEM_HEIGHT - 1, screenWidth, itemY + LIST_ITEM_HEIGHT - 1);
    }
  }
}

void NewsActivity::renderDownloadStatus() const {
  const int screenWidth = renderer.getScreenWidth();
  const int centerY = TAB_BAR_HEIGHT + HEADER_HEIGHT + 80;

  const char* statusText = "";
  switch (downloadState) {
    case DownloadState::CONNECTING:
      statusText = "Connecting to WiFi...";
      break;
    case DownloadState::DOWNLOADING:
      statusText = "Downloading...";
      break;
    case DownloadState::SUCCESS:
      statusText = "Download complete!";
      break;
    case DownloadState::ALREADY_EXISTS:
      statusText = "Today's news already downloaded.";
      break;
    case DownloadState::ERROR:
      statusText = "Download failed.";
      break;
    default:
      break;
  }

  const int textW = renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_10_FONT_ID, statusText);
  renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, (screenWidth - textW) / 2, centerY, statusText);

  if (downloadState == DownloadState::DOWNLOADING) {
    const int barX = 40;
    const int barY = centerY + 40;
    const int barW = screenWidth - 80;
    const int barH = 12;
    renderer.rectangle.render(barX, barY, barW, barH);
    if (downloadTotal > 0) {
      int fillW = (downloadProgress * barW) / downloadTotal;
      if (fillW > 0) {
        renderer.rectangle.fill(barX + 1, barY + 1, fillW - 2, barH - 2, static_cast<int>(GfxRenderer::FillTone::Ink));
      }
      int pct = (downloadProgress * 100) / downloadTotal;
      char pctText[32];
      snprintf(pctText, sizeof(pctText), "%d%%  (%d KB)", pct, downloadProgress / 1024);
      const int pctW = renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_8_FONT_ID, pctText);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, (screenWidth - pctW) / 2, barY + barH + 8, pctText);
    }
  }

  if (!downloadError.empty()) {
    const int maxW = screenWidth - 40;
    const int lineH = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_8_FONT_ID) + 2;
    int y = centerY + 60;
    std::string remaining = downloadError;
    while (!remaining.empty() && y < renderer.getScreenHeight() - 60) {
      std::string line = remaining;
      while (renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_8_FONT_ID, line.c_str()) > maxW && line.size() > 1) {
        line.pop_back();
      }
      renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, 20, y, line.c_str());
      remaining = remaining.substr(line.size());
      y += lineH;
    }
  }

  if (downloadState == DownloadState::SUCCESS || downloadState == DownloadState::ERROR ||
      downloadState == DownloadState::ALREADY_EXISTS) {
    const char* hint = "Press any button to continue";
    const int hintW = renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_8_FONT_ID, hint);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, (screenWidth - hintW) / 2, centerY + 100, hint);
  }
}

void NewsActivity::renderActionMenu() const {
  const int screenWidth = renderer.getScreenWidth();
  const int menuY = TAB_BAR_HEIGHT + HEADER_HEIGHT + 20;

  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(entries.size())) return;

  const auto& entry = entries[selectedIndex];
  renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 20, menuY, entry.displayDate.c_str(), true,
                       EpdFontFamily::BOLD);

  const char* actions[] = {"Delete", entry.bookmarked ? "Unbookmark" : "Bookmark", "Archive"};

  for (int i = 0; i < 3; i++) {
    const int itemY = menuY + 40 + i * LIST_ITEM_HEIGHT;
    const bool isSelected = (i == actionMenuIndex);

    if (isSelected) {
      renderer.rectangle.fill(0, itemY, screenWidth, LIST_ITEM_HEIGHT, static_cast<int>(GfxRenderer::FillTone::Ink));
    }

    const int textY = itemY + (LIST_ITEM_HEIGHT - renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID)) / 2;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 40, textY, actions[i], !isSelected);
  }
}

void NewsActivity::startDownload() {
  std::string destPath = std::string(NEWS_DIR) + "/" + todayFilename();

  if (SdMan.exists(destPath.c_str())) {
    downloadState = DownloadState::ALREADY_EXISTS;
    render();
    return;
  }

  // also check bookmark dir
  std::string bookmarkPath = std::string(BOOKMARK_DIR) + "/" + todayFilename();
  if (SdMan.exists(bookmarkPath.c_str())) {
    downloadState = DownloadState::ALREADY_EXISTS;
    render();
    return;
  }

  downloadState = DownloadState::CONNECTING;
  render();

#ifndef SIMULATOR
  if (WiFi.status() != WL_CONNECTED) {
    WIFI_STORE.loadFromFile();
    const WifiCredential* cred = WIFI_STORE.getLastCredential();
    if (!cred) {
      downloadState = DownloadState::ERROR;
      downloadError = "No saved WiFi network.";
      render();
      return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (WiFi.status() != WL_CONNECTED) {
      downloadState = DownloadState::ERROR;
      downloadError = "WiFi connection failed.";
      WiFi.disconnect(true);
      render();
      return;
    }
  }
#endif

  downloadState = DownloadState::DOWNLOADING;
  downloadProgress = 0;
  downloadTotal = 0;
  render();

  SdMan.mkdir(NEWS_DIR);
  std::string url = buildDownloadUrl();
  Serial.printf("[NEWS] Downloading: %s\n", url.c_str());
  Serial.printf("[NEWS] Dest: %s\n", destPath.c_str());
  Serial.printf("[NEWS] Free heap: %u\n", ESP.getFreeHeap());

  unsigned long lastRenderMs = 0;
  auto result =
      HttpDownloader::downloadToFile(url, destPath, "", "", [this, &lastRenderMs](size_t downloaded, size_t total) {
        downloadProgress = static_cast<int>(downloaded);
        downloadTotal = static_cast<int>(total);
        unsigned long now = millis();
        if (now - lastRenderMs > 500) {
          lastRenderMs = now;
          render();
        }
      });

#ifndef SIMULATOR
  WiFi.disconnect(true);
#endif

  {
    FsFile log;
    if (SdMan.openFileForWrite("NEWS", "/News/.download.log", log)) {
      char logBuffer[1024];
      const int length = snprintf(logBuffer, sizeof(logBuffer),
                                  "url: %s\ndest: %s\nresult: %d\nheap_free: %u\ndownloaded: %d / %d\n",
                                  url.c_str(), destPath.c_str(), static_cast<int>(result), ESP.getFreeHeap(),
                                  downloadProgress, downloadTotal);
      if (length > 0) {
        const size_t bytes = std::min(static_cast<size_t>(length), sizeof(logBuffer) - 1);
        log.write(reinterpret_cast<const uint8_t*>(logBuffer), bytes);
      }
      log.close();
    }
  }

  if (result == HttpDownloader::OK) {
    downloadState = DownloadState::SUCCESS;
    scanNewsFolder();
  } else {
    downloadState = DownloadState::ERROR;
    downloadError = "Failed (code " + std::to_string(result) + "): " + url;
  }
  render();
}

bool NewsActivity::tryAutoDownload() {
  if (!SETTINGS.newsAutoDownload) return false;

#ifdef SIMULATOR
  return false;
#else
  HalGPIO::DateTime dt;
  if (!gpio.readDateTime(dt) || dt.year < 2024) return false;
  if (dt.hour < SETTINGS.newsDownloadHour) return false;

  std::string filename = todayFilename();
  std::string destPath = std::string(NEWS_DIR) + "/" + filename;
  std::string bookmarkPath = std::string(BOOKMARK_DIR) + "/" + filename;

  if (SdMan.exists(destPath.c_str()) || SdMan.exists(bookmarkPath.c_str())) return false;

  WIFI_STORE.loadFromFile();
  const WifiCredential* cred = WIFI_STORE.getLastCredential();
  if (!cred) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true);
    return false;
  }

  SdMan.mkdir(NEWS_DIR);
  std::string url = buildDownloadUrl();
  auto result = HttpDownloader::downloadToFile(url, destPath);

  WiFi.disconnect(true);
  return result == HttpDownloader::OK;
#endif
}

void NewsActivity::deleteEntry(int index) {
  if (index < 0 || index >= static_cast<int>(entries.size())) return;
  SdMan.remove(entries[index].path.c_str());
}

void NewsActivity::archiveEntry(int index) {
  if (index < 0 || index >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[index];
  std::string destPath = std::string(ARCHIVE_DIR) + "/" + entry.filename;
  SdMan.rename(entry.path.c_str(), destPath.c_str());
}

void NewsActivity::bookmarkEntry(int index) {
  if (index < 0 || index >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[index];
  std::string destPath = std::string(BOOKMARK_DIR) + "/" + entry.filename;
  SdMan.rename(entry.path.c_str(), destPath.c_str());
}

void NewsActivity::unbookmarkEntry(int index) {
  if (index < 0 || index >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[index];
  std::string destPath = std::string(NEWS_DIR) + "/" + entry.filename;
  SdMan.rename(entry.path.c_str(), destPath.c_str());
}

void NewsActivity::unarchiveEntry(int index) {
  if (index < 0 || index >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[index];
  std::string destPath = std::string(NEWS_DIR) + "/" + entry.filename;
  SdMan.rename(entry.path.c_str(), destPath.c_str());
}

void NewsActivity::loadState() {}

void NewsActivity::saveState() {}
