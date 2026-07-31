#pragma once

#include <cstdint>
#include <vector>

struct ReadingDailyEntry {
  uint32_t dayOrdinal = 0;
  uint32_t readingMs = 0;
};

struct ReadingDailySummary {
  bool hasClock = false;
  uint32_t todayDayOrdinal = 0;
  uint32_t todayReadingMs = 0;
  uint32_t recent7ReadingMs = 0;
  uint32_t recent30ReadingMs = 0;
  uint32_t currentGoalStreakDays = 0;
  uint32_t maxGoalStreakDays = 0;
  uint32_t goalReadingMs = 0;
};

class ReadingDailyStats {
 public:
  static bool recordReadingMs(uint32_t readingMs);
  static ReadingDailySummary loadSummary();
  static uint32_t getDailyGoalMs();

 private:
  static bool loadEntries(std::vector<ReadingDailyEntry>& entries);
  static bool saveEntries(const std::vector<ReadingDailyEntry>& entries);
};
