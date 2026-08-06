#pragma once

/**
 * @file Statistics.h
 * @brief Public interface and types for Statistics.
 */

#include <cstdint>
#include <string>
#include <vector>

/**
 * Structure containing reading statistics for a single book.
 *
 * This structure tracks reading progress, time spent, pages read,
 * and other metrics for a specific EPUB book.
 */
struct BookReadingStats {
  std::string path;    ///< Full filesystem path to the book file
  std::string title;   ///< Title of the book from EPUB metadata
  std::string author;  ///< Author of the book from EPUB metadata

  uint32_t totalReadingTimeMs;  ///< Total time spent reading in milliseconds
  uint32_t totalPagesRead;      ///< Total number of pages turned
  uint32_t totalChaptersRead;   ///< Number of chapters fully read
  uint32_t lastReadTimeMs;      ///< Timestamp of last reading session
  float progressPercent;        ///< Current reading progress (0.0-100.0)
  uint16_t lastSpineIndex;      ///< Index of last accessed chapter
  uint16_t lastPageNumber;      ///< Page number within last chapter
  uint32_t avgPageTimeMs;       ///< Average time per page in milliseconds
  uint32_t sessionCount;        ///< Number of reading sessions for this book

  /**
   * Default constructor initializing all numeric fields to zero
   * and string fields to empty.
   */
  BookReadingStats()
      : path(""),
        title(""),
        author(""),
        totalReadingTimeMs(0),
        totalPagesRead(0),
        totalChaptersRead(0),
        lastReadTimeMs(0),
        progressPercent(0),
        lastSpineIndex(0),
        lastPageNumber(0),
        avgPageTimeMs(0),
        sessionCount(0) {}
};

/**
 * Structure containing aggregated reading statistics across all books.
 */
struct GlobalReadingStats {
  uint32_t totalBooksStarted;   ///< Total number of books ever opened
  uint32_t totalBooksFinished;  ///< Total number of books completed (progress >= 99.9%)
  uint32_t totalReadingTimeMs;  ///< Total reading time across all books in milliseconds
  uint32_t totalPagesRead;      ///< Total pages turned across all books
  uint32_t totalChaptersRead;   ///< Total chapters read across all books
  uint32_t totalSessions;       ///< Total number of reading sessions across all books

  /**
   * Default constructor initializing all fields to zero.
   */
  GlobalReadingStats()
      : totalBooksStarted(0),
        totalBooksFinished(0),
        totalReadingTimeMs(0),
        totalPagesRead(0),
        totalChaptersRead(0),
        totalSessions(0) {}
};

namespace ReadingStats {

/** Initial dwell time required before enough per-book history exists to learn a reading pace. */
constexpr uint32_t PAGE_READ_BOOTSTRAP_MS = 8000;
/** Bounds keep unusually fast or idle pages from making the adaptive threshold unreasonable. */
constexpr uint32_t PAGE_READ_MIN_MS = 5000;
constexpr uint32_t PAGE_READ_MAX_MS = 20000;
constexpr uint32_t PAGE_READ_MIN_SAMPLES = 3;

/**
 * Returns the minimum time a page must remain visible before it counts as read.
 *
 * Once a book has enough history, 20% of its learned average is used. This rejects quick peeks and
 * immediate backtracking without requiring every legitimately read page to match the full average.
 */
inline uint32_t pageReadThresholdMs(const BookReadingStats& stats) {
  if (stats.totalPagesRead < PAGE_READ_MIN_SAMPLES || stats.avgPageTimeMs == 0) {
    return PAGE_READ_BOOTSTRAP_MS;
  }

  const uint32_t adaptiveMs = stats.avgPageTimeMs / 5;
  return adaptiveMs < PAGE_READ_MIN_MS ? PAGE_READ_MIN_MS
                                       : (adaptiveMs > PAGE_READ_MAX_MS ? PAGE_READ_MAX_MS : adaptiveMs);
}

inline bool qualifiesAsPageRead(const uint32_t timeSpentMs, const BookReadingStats& stats) {
  return timeSpentMs >= pageReadThresholdMs(stats);
}

/**
 * Adds one qualifying sample to the learned page-reading average.
 *
 * totalReadingTimeMs deliberately remains independent: time spent browsing short pages still belongs
 * to the reading session, while only genuine reads influence the pace used by the qualification rule.
 */
inline void recordQualifiedPage(const uint32_t timeSpentMs, BookReadingStats& stats) {
  const uint32_t previousSamples = stats.avgPageTimeMs == 0 ? 0 : stats.totalPagesRead;
  const uint64_t previousQualifiedTime =
      static_cast<uint64_t>(stats.avgPageTimeMs) * static_cast<uint64_t>(previousSamples);
  stats.totalPagesRead++;
  stats.avgPageTimeMs =
      static_cast<uint32_t>((previousQualifiedTime + timeSpentMs) / static_cast<uint64_t>(previousSamples + 1));
}

}  // namespace ReadingStats

/**
 * Saves reading statistics for a book to its cache directory.
 *
 * @param cachePath Path to the book's cache directory
 * @param stats The book reading statistics to persist
 */
void saveBookStats(const char* cachePath, const BookReadingStats& stats);

/**
 * Loads reading statistics for a book from its cache directory.
 *
 * @param cachePath Path to the book's cache directory
 * @param stats Reference to populate with loaded statistics
 * @return true if statistics were successfully loaded, false otherwise
 */
bool loadBookStats(const char* cachePath, BookReadingStats& stats);

/**
 * Retrieves reading statistics for all books with per-book stats on the SD card.
 *
 * Scans `/.metadata/epub` (EPUB caches) and `/.metadata/xtc` (XTC caches)
 * for `statistics.bin` files.
 *
 * @return Vector containing statistics for all books with valid stats files
 */
std::vector<BookReadingStats> getAllBooksStats();

/**
 * Retrieves reading statistics for a specific book.
 *
 * @param bookPath Path to the book's cache directory
 * @param stats Reference to populate with the book's statistics
 * @return true if statistics were successfully loaded, false otherwise
 */
bool getBookStats(const char* bookPath, BookReadingStats& stats);

/**
 * Loads global reading statistics from the statistics file.
 *
 * @param stats Reference to populate with global statistics
 * @return true if statistics were successfully loaded, false otherwise
 */
bool loadGlobalStats(GlobalReadingStats& stats);

/**
 * Saves global reading statistics to the statistics file.
 *
 * @param stats The global statistics to persist
 */
void saveGlobalStats(const GlobalReadingStats& stats);

/** Aggregates global totals from an already-built list of per-book stats (no SD rescan). */
GlobalReadingStats aggregateGlobalStatsFromBooks(const std::vector<BookReadingStats>& books);

/**
 * Recomputes global totals by scanning all per-book statistics files.
 */
GlobalReadingStats generateGlobalStats();

/**
 * Backs up every book's stats to a single human-editable JSON file at
 * `<backupRoot>/reading_stats.json` (reading time exposed as whole seconds).
 *
 * @param backupRoot Destination root (e.g., "/.backups/reading_stats")
 * @return Number of books written
 */
int backupAllBookStats(const char* backupRoot);

/**
 * Restores stats from `<backupRoot>/reading_stats.json` back into the per-book cache dirs, replacing
 * current values. Use this to carry reading time across firmware flashes, hand-edit reading time, or
 * import stats from another device.
 *
 * @param backupRoot Source root (e.g., "/.backups/reading_stats")
 * @return Number of books restored
 */
int restoreAllBookStats(const char* backupRoot);
