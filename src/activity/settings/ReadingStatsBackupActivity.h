#pragma once

/**
 * @file ReadingStatsBackupActivity.h
 * @brief Backup/restore of per-book reading statistics to the SD card.
 *
 * Copies each book's statistics.bin to /.backups/reading_stats (mirroring the cache layout) and
 * restores them back. This keeps reading time (BookReadingStats::totalReadingTimeMs) across firmware
 * flashes and lets stats recorded on another firmware/device be carried in.
 */

#include <functional>
#include <string>

#include "activity/ActivityWithSubactivity.h"

class ReadingStatsBackupActivity final : public ActivityWithSubactivity {
 public:
  ReadingStatsBackupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& onBack, const int initialSelection = 0)
      : ActivityWithSubactivity("ReadingStatsBackup", renderer, mappedInput),
        selected(initialSelection == 1 ? 1 : 0),
        onBack(onBack) {}

  void onEnter() override;
  void loop() override;

 private:
  enum class State { CHOOSE, DONE };

  /** Root under which per-book stats mirrors are stored. */
  static constexpr const char* kBackupRoot = "/.backups/reading_stats";

  State state = State::CHOOSE;
  int selected = 0;  ///< 0 = Backup, 1 = Restore
  std::string resultMessage;
  std::function<void()> onBack;

  void render();
  void runSelected();
};
