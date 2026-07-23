#include "state/ReadingDailyStats.h"

#include <HalGPIO.h>
#include <SDCardManager.h>
#include <Serialization.h>

#include <algorithm>

#include "state/SystemSetting.h"

namespace {
constexpr char DAILY_STATS_FILE[] = "/.metadata/reading_daily.bin";
constexpr uint8_t DAILY_STATS_VERSION = 1;
constexpr uint16_t MAX_DAILY_ENTRIES = 730;

bool isLeapYear(const uint16_t year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint16_t dayOfYear(const HalGPIO::DateTime& dt) {
  static constexpr uint16_t daysBeforeMonth[] = {0,  0,  31, 59, 90, 120, 151,
                                                 181, 212, 243, 273, 304, 334};
  uint16_t doy = daysBeforeMonth[dt.month] + dt.day;
  if (dt.month > 2 && isLeapYear(dt.year)) {
    ++doy;
  }
  return doy;
}

uint32_t dayOrdinalFromDate(const HalGPIO::DateTime& dt) {
  if (dt.year < 2024 || dt.year > 2099 || dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31) {
    return 0;
  }
  uint32_t days = 0;
  for (uint16_t year = 2024; year < dt.year; ++year) {
    days += isLeapYear(year) ? 366 : 365;
  }
  return days + dayOfYear(dt);
}

uint32_t currentRtcDayOrdinal() {
  if (!gpio.deviceIsX3()) {
    return 0;
  }
  HalGPIO::DateTime dt;
  if (!gpio.readDateTime(dt)) {
    return 0;
  }
  return dayOrdinalFromDate(dt);
}

bool countsForGoal(const ReadingDailyEntry& entry, const uint32_t goalMs) {
  return entry.dayOrdinal != 0 && goalMs > 0 && entry.readingMs >= goalMs;
}
}  // namespace

bool ReadingDailyStats::loadEntries(std::vector<ReadingDailyEntry>& entries) {
  entries.clear();
  FsFile file;
  if (!SdMan.openFileForRead("RDS", DAILY_STATS_FILE, file)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  serialization::readPod(file, version);
  serialization::readPod(file, count);
  if (version != DAILY_STATS_VERSION || count > MAX_DAILY_ENTRIES) {
    file.close();
    return false;
  }

  entries.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    ReadingDailyEntry entry;
    serialization::readPod(file, entry.dayOrdinal);
    serialization::readPod(file, entry.readingMs);
    if (entry.dayOrdinal != 0) {
      entries.push_back(entry);
    }
  }
  file.close();
  std::sort(entries.begin(), entries.end(),
            [](const ReadingDailyEntry& a, const ReadingDailyEntry& b) { return a.dayOrdinal < b.dayOrdinal; });
  return true;
}

bool ReadingDailyStats::saveEntries(const std::vector<ReadingDailyEntry>& entries) {
  SdMan.mkdir("/.metadata");
  FsFile file;
  if (!SdMan.openFileForWrite("RDS", DAILY_STATS_FILE, file)) {
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(entries.size(), MAX_DAILY_ENTRIES));
  serialization::writePod(file, DAILY_STATS_VERSION);
  serialization::writePod(file, count);
  const size_t start = entries.size() > count ? entries.size() - count : 0;
  for (size_t i = start; i < entries.size(); ++i) {
    serialization::writePod(file, entries[i].dayOrdinal);
    serialization::writePod(file, entries[i].readingMs);
  }
  file.close();
  return true;
}

bool ReadingDailyStats::recordReadingMs(const uint32_t readingMs) {
  if (readingMs == 0) {
    return false;
  }
  const uint32_t today = currentRtcDayOrdinal();
  if (today == 0) {
    return false;
  }

  std::vector<ReadingDailyEntry> entries;
  loadEntries(entries);
  auto it = std::lower_bound(entries.begin(), entries.end(), today,
                             [](const ReadingDailyEntry& entry, uint32_t day) { return entry.dayOrdinal < day; });
  if (it == entries.end() || it->dayOrdinal != today) {
    ReadingDailyEntry entry;
    entry.dayOrdinal = today;
    entry.readingMs = 0;
    it = entries.insert(it, entry);
  }
  const uint64_t sum = static_cast<uint64_t>(it->readingMs) + readingMs;
  it->readingMs = static_cast<uint32_t>(std::min<uint64_t>(sum, UINT32_MAX));
  if (entries.size() > MAX_DAILY_ENTRIES) {
    entries.erase(entries.begin(), entries.begin() + static_cast<long>(entries.size() - MAX_DAILY_ENTRIES));
  }
  return saveEntries(entries);
}

ReadingDailySummary ReadingDailyStats::loadSummary() {
  ReadingDailySummary summary;
  summary.goalReadingMs = getDailyGoalMs();
  summary.todayDayOrdinal = currentRtcDayOrdinal();
  summary.hasClock = summary.todayDayOrdinal != 0;
  if (!summary.hasClock) {
    return summary;
  }

  std::vector<ReadingDailyEntry> entries;
  loadEntries(entries);
  const uint32_t start7 = summary.todayDayOrdinal > 6 ? summary.todayDayOrdinal - 6 : 0;
  const uint32_t start30 = summary.todayDayOrdinal > 29 ? summary.todayDayOrdinal - 29 : 0;
  uint32_t currentRun = 0;
  uint32_t previousGoalDay = 0;
  for (const auto& entry : entries) {
    if (entry.dayOrdinal == summary.todayDayOrdinal) {
      summary.todayReadingMs = entry.readingMs;
    }
    if (entry.dayOrdinal >= start7 && entry.dayOrdinal <= summary.todayDayOrdinal) {
      summary.recent7ReadingMs += entry.readingMs;
    }
    if (entry.dayOrdinal >= start30 && entry.dayOrdinal <= summary.todayDayOrdinal) {
      summary.recent30ReadingMs += entry.readingMs;
    }
    if (!countsForGoal(entry, summary.goalReadingMs)) {
      continue;
    }
    if (previousGoalDay != 0 && entry.dayOrdinal == previousGoalDay + 1) {
      ++currentRun;
    } else {
      currentRun = 1;
    }
    previousGoalDay = entry.dayOrdinal;
    summary.maxGoalStreakDays = std::max(summary.maxGoalStreakDays, currentRun);
  }

  if (previousGoalDay == summary.todayDayOrdinal ||
      (summary.todayDayOrdinal > 0 && previousGoalDay + 1 == summary.todayDayOrdinal)) {
    summary.currentGoalStreakDays = currentRun;
  }
  return summary;
}

uint32_t ReadingDailyStats::getDailyGoalMs() {
  switch (SETTINGS.dailyReadingGoal) {
    case SystemSetting::DAILY_GOAL_15_MIN:
      return 15UL * 60UL * 1000UL;
    case SystemSetting::DAILY_GOAL_45_MIN:
      return 45UL * 60UL * 1000UL;
    case SystemSetting::DAILY_GOAL_60_MIN:
      return 60UL * 60UL * 1000UL;
    case SystemSetting::DAILY_GOAL_30_MIN:
    default:
      return 30UL * 60UL * 1000UL;
  }
}
