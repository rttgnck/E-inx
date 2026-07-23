#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "../Menu.h"

struct NewsEntry {
  std::string filename;
  std::string displayDate;
  std::string path;
  bool bookmarked = false;
  bool archived = false;
};

class NewsActivity final : public Activity, public Menu {
 public:
  explicit NewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        const std::function<void()>& onRecentOpen,
                        const std::function<void()>& onLibraryOpen,
                        const std::function<void(const std::string& path)>& onOpenBook,
                        const std::function<void()>& onGoToNews)
      : Activity("News", renderer, mappedInput),
        Menu(),
        onRecentOpen(onRecentOpen),
        onLibraryOpen(onLibraryOpen),
        onOpenBook(onOpenBook),
        onGoToNews(onGoToNews) {
    tabSelectorIndex = 1;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;

  enum class DownloadState { IDLE, CONNECTING, DOWNLOADING, SUCCESS, ALREADY_EXISTS, ERROR };

  void startDownload();

  static bool tryAutoDownload();

 private:
  static constexpr const char* NEWS_DIR = "/News";
  static constexpr const char* ARCHIVE_DIR = "/News/.archive";
  static constexpr const char* BOOKMARK_DIR = "/News/.bookmarked";
  static constexpr const char* STATE_FILE = "/News/.news_state.json";

  const std::function<void()> onRecentOpen;
  const std::function<void()> onLibraryOpen;
  const std::function<void(const std::string& path)> onOpenBook;
  const std::function<void()> onGoToNews;

  std::vector<NewsEntry> entries;
  int selectedIndex = 0;
  int scrollOffset = 0;
  bool updateRequired = false;
  bool actionMenuOpen = false;
  int actionMenuIndex = 0;

  DownloadState downloadState = DownloadState::IDLE;
  int downloadProgress = 0;
  int downloadTotal = 0;
  std::string downloadError;

  void scanNewsFolder();
  void loadState();
  void saveState();
  void render() const;
  void renderList() const;
  void renderDownloadStatus() const;
  void renderActionMenu() const;

  void deleteEntry(int index);
  void archiveEntry(int index);
  void bookmarkEntry(int index);
  void unarchiveEntry(int index);
  void unbookmarkEntry(int index);

  int getVisibleItems() const;
  static std::string todayFilename();
  static std::string buildDownloadUrl();

  void navigateToSelectedMenu() override {
    if (tabSelectorIndex == 0) onRecentOpen();
    if (tabSelectorIndex == 2) onLibraryOpen();
  }
};
