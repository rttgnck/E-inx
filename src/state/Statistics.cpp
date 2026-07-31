/**
 * @file Statistics.cpp
 * @brief Definitions for Statistics.
 */

#include "state/Statistics.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <SDCardManager.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

static const char* STATS_DIR = "/.system";
static const char* statistics_FILE = "/.system/statistics.bin";

constexpr uint32_t STATS_FILE_VERSION = 2;
constexpr uint32_t STATS_MAGIC_NUMBER = 0x53544154;

/**
 * RAII wrapper for FsFile that ensures file is closed when object goes out of scope.
 */
class FileGuard {
 private:
  FsFile& file;

 public:
  explicit FileGuard(FsFile& f) : file(f) {}
  ~FileGuard() {
    if (file) {
      file.close();
    }
  }
  FileGuard(const FileGuard&) = delete;
  FileGuard& operator=(const FileGuard&) = delete;
};

/**
 * Saves reading statistics for a specific book to a binary file in its cache directory.
 * Uses a temporary file and atomic rename to prevent corruption.
 *
 * @param cachePath Path to the book's cache directory
 * @param stats The book reading statistics to save
 */
void saveBookStats(const char* cachePath, const BookReadingStats& stats) {
  if (!cachePath) return;
  std::string statsPath = std::string(cachePath) + "/statistics.bin";
  std::string tempPath = statsPath + ".tmp";

  bool writeSuccess = false;

  {
    FsFile file;
    FileGuard guard(file);

    if (SdMan.openFileForWrite("STATS", tempPath.c_str(), file)) {
      uint32_t magic = STATS_MAGIC_NUMBER;
      uint32_t version = STATS_FILE_VERSION;

      file.write(&magic, sizeof(uint32_t));
      file.write(&version, sizeof(uint32_t));

      file.write(&stats.totalReadingTimeMs, sizeof(uint32_t));
      file.write(&stats.totalPagesRead, sizeof(uint32_t));
      file.write(&stats.totalChaptersRead, sizeof(uint32_t));
      file.write(&stats.lastReadTimeMs, sizeof(uint32_t));
      file.write(&stats.progressPercent, sizeof(float));
      file.write(&stats.lastSpineIndex, sizeof(uint16_t));
      file.write(&stats.lastPageNumber, sizeof(uint16_t));
      file.write(&stats.avgPageTimeMs, sizeof(uint32_t));
      file.write(&stats.sessionCount, sizeof(uint32_t));

      uint32_t pathLength = stats.path.length();
      file.write(&pathLength, sizeof(uint32_t));
      if (pathLength > 0) file.write(stats.path.c_str(), pathLength);

      uint32_t titleLength = stats.title.length();
      file.write(&titleLength, sizeof(uint32_t));
      if (titleLength > 0) file.write(stats.title.c_str(), titleLength);

      uint32_t authorLength = stats.author.length();
      file.write(&authorLength, sizeof(uint32_t));
      if (authorLength > 0) file.write(stats.author.c_str(), authorLength);

      writeSuccess = true;
    }
  }

  if (writeSuccess) {
    SdMan.remove(statsPath.c_str());
    if (!SdMan.rename(tempPath.c_str(), statsPath.c_str())) {
      SdMan.remove(tempPath.c_str());
    }
  } else {
    SdMan.remove(tempPath.c_str());
  }
}

/**
 * Loads reading statistics for a specific book from its cache directory.
 * Validates file format using magic number and version.
 *
 * @param cachePath Path to the book's cache directory
 * @param stats Reference to populate with loaded statistics
 * @return true if statistics were successfully loaded, false otherwise
 */
bool loadBookStats(const char* cachePath, BookReadingStats& stats) {
  if (!cachePath) return false;
  std::string statsPath = std::string(cachePath) + "/statistics.bin";

  FsFile file;
  FileGuard guard(file);

  if (!SdMan.openFileForRead("STATS", statsPath.c_str(), file)) {
    return false;
  }

  uint32_t magic, version;
  if (file.read(&magic, sizeof(uint32_t)) != sizeof(uint32_t) || magic != STATS_MAGIC_NUMBER) {
    return false;
  }

  if (file.read(&version, sizeof(uint32_t)) != sizeof(uint32_t)) {
    return false;
  }

  file.read(&stats.totalReadingTimeMs, sizeof(uint32_t));
  file.read(&stats.totalPagesRead, sizeof(uint32_t));
  file.read(&stats.totalChaptersRead, sizeof(uint32_t));
  file.read(&stats.lastReadTimeMs, sizeof(uint32_t));
  file.read(&stats.progressPercent, sizeof(float));
  file.read(&stats.lastSpineIndex, sizeof(uint16_t));
  file.read(&stats.lastPageNumber, sizeof(uint16_t));
  file.read(&stats.avgPageTimeMs, sizeof(uint32_t));

  if (version >= 2) {
    file.read(&stats.sessionCount, sizeof(uint32_t));
  } else {
    stats.sessionCount = 0;
  }

  uint32_t pathLen;
  if (file.read(&pathLen, sizeof(uint32_t)) == sizeof(uint32_t) && pathLen < 512) {
    if (pathLen > 0) {
      stats.path.resize(pathLen);
      file.read(&stats.path[0], pathLen);
    } else {
      stats.path.clear();
    }
  }

  uint32_t titleLen;
  if (file.read(&titleLen, sizeof(uint32_t)) == sizeof(uint32_t) && titleLen < 512) {
    if (titleLen > 0) {
      stats.title.resize(titleLen);
      file.read(&stats.title[0], titleLen);
    } else {
      stats.title.clear();
    }
  }

  uint32_t authorLen;
  if (file.read(&authorLen, sizeof(uint32_t)) == sizeof(uint32_t) && authorLen < 512) {
    if (authorLen > 0) {
      stats.author.resize(authorLen);
      file.read(&stats.author[0], authorLen);
    } else {
      stats.author.clear();
    }
  }

  return true;
}

namespace {

void appendStatsFromCacheDir(std::vector<BookReadingStats>& result, const char* rootDir,
                             bool (*acceptName)(const char* name)) {
  FsFile root;
  FileGuard rootGuard(root);

  root = SdMan.open(rootDir);
  if (!root || !root.isDirectory()) {
    return;
  }

  root.rewindDirectory();
  char fileName[128];

  while (true) {
    FsFile entry;
    FileGuard entryGuard(entry);

    entry = root.openNextFile();
    if (!entry) {
      break;
    }

    if (!entry.isDirectory()) {
      continue;
    }

    entry.getName(fileName, sizeof(fileName));
    if (acceptName != nullptr && !acceptName(fileName)) {
      continue;
    }

    const std::string path = std::string(rootDir) + "/" + std::string(fileName);
    BookReadingStats stats;
    if (loadBookStats(path.c_str(), stats)) {
      stats.path = path;
      result.push_back(stats);
    }
  }
}

}  // namespace

/**
 * Retrieves reading statistics for all books in the EPUB cache directory.
 * Scans through all subdirectories in /.metadata/epub and loads stats for each.
 *
 * @return Vector containing statistics for all books that have saved stats
 */
std::vector<BookReadingStats> getAllBooksStats() {
  std::vector<BookReadingStats> result;
  result.reserve(24);

  appendStatsFromCacheDir(result, "/.metadata/epub", nullptr);
  appendStatsFromCacheDir(result, "/.metadata/xtc", nullptr);

  return result;
}

/**
 * Retrieves reading statistics for a specific book.
 *
 * @param bookPath Path to the book's cache directory
 * @param stats Reference to populate with the book's statistics
 * @return true if statistics were successfully loaded, false otherwise
 */
bool getBookStats(const char* bookPath, BookReadingStats& stats) { return loadBookStats(bookPath, stats); }

/**
 * Loads global reading statistics from the main statistics file.
 * Zeros out the stats struct if read fails.
 *
 * @param stats Reference to populate with global statistics
 * @return true if statistics were successfully loaded, false otherwise
 */
bool loadGlobalStats(GlobalReadingStats& stats) {
  memset(&stats, 0, sizeof(GlobalReadingStats));
  static const char* const kLegacyGlobalPath = ".system/statistics.bin";
  {
    FsFile file;
    FileGuard guard(file);
    if (SdMan.openFileForRead("STATS", statistics_FILE, file)) {
      const size_t n = file.read(&stats, sizeof(GlobalReadingStats));
      return n == sizeof(GlobalReadingStats);
    }
  }
  {
    FsFile file;
    FileGuard guard(file);
    if (!SdMan.openFileForRead("STATS", kLegacyGlobalPath, file)) {
      return false;
    }
    const size_t n = file.read(&stats, sizeof(GlobalReadingStats));
    if (n != sizeof(GlobalReadingStats)) {
      return false;
    }
  }
  saveGlobalStats(stats);
  SdMan.remove(kLegacyGlobalPath);
  return true;
}

/**
 * Saves global reading statistics to the main statistics file.
 * Uses a temporary file and atomic rename to prevent corruption.
 * Creates the statistics directory if it doesn't exist.
 *
 * @param stats Global statistics to save
 */
void saveGlobalStats(const GlobalReadingStats& stats) {
  SdMan.mkdir(STATS_DIR);  // same root as settings.bin, wifi.bin, etc.
  const std::string tempPath = std::string(statistics_FILE) + ".tmp";
  bool writeOk = false;
  {
    FsFile file;
    FileGuard guard(file);
    if (SdMan.openFileForWrite("STATS", tempPath.c_str(), file)) {
      const size_t n = file.write(&stats, sizeof(GlobalReadingStats));
      writeOk = (n == sizeof(GlobalReadingStats));
    }
  }
  if (writeOk) {
    SdMan.remove(statistics_FILE);
    if (!SdMan.rename(tempPath.c_str(), statistics_FILE)) {
      SdMan.remove(tempPath.c_str());
    }
  } else {
    SdMan.remove(tempPath.c_str());
  }
}

/**
 * Generates global reading statistics by aggregating all individual book statistics.
 * Calculates totals for books started, finished, reading time, pages, chapters, and sessions.
 *
 * @return Aggregated global reading statistics
 */
GlobalReadingStats aggregateGlobalStatsFromBooks(const std::vector<BookReadingStats>& allBooks) {
  GlobalReadingStats global;
  memset(&global, 0, sizeof(GlobalReadingStats));
  for (const auto& book : allBooks) {
    global.totalBooksStarted++;
    global.totalReadingTimeMs += book.totalReadingTimeMs;
    global.totalPagesRead += book.totalPagesRead;
    global.totalChaptersRead += book.totalChaptersRead;
    global.totalSessions += book.sessionCount;
    if (book.progressPercent >= 99.0f) {
      global.totalBooksFinished++;
    }
  }
  return global;
}

GlobalReadingStats generateGlobalStats() { return aggregateGlobalStatsFromBooks(getAllBooksStats()); }

namespace {

/** Per-book cache roots that hold a `statistics.bin`, and their type tag used in the JSON backup. */
struct StatsCacheRoot {
  const char* cacheRoot;  ///< e.g. "/.metadata/epub"
  const char* typeTag;    ///< value written to the "type" field, e.g. "epub"
};
const StatsCacheRoot kStatsCacheRoots[] = {{"/.metadata/epub", "epub"}, {"/.metadata/xtc", "xtc"}};

/** Human-editable backup file holding all books' stats. Lives directly under the backup root. */
const char* kReadingStatsJson = "reading_stats.json";

/** Resolves a "type" tag ("epub"/"xtc") to its cache root, or nullptr if unknown. */
const char* cacheRootForType(const char* type) {
  if (!type) return nullptr;
  for (const auto& r : kStatsCacheRoots) {
    if (strcmp(type, r.typeTag) == 0) return r.cacheRoot;
  }
  return nullptr;
}

/**
 * Iterates each `<hash>` subdirectory of `parentDir` that contains a `statistics.bin`, invoking
 * `fn(hashName)`. Returns the number of invocations that returned true.
 */
int forEachStatsSubdir(const char* parentDir, const std::function<bool(const std::string&)>& fn) {
  FsFile root;
  FileGuard rootGuard(root);
  root = SdMan.open(parentDir);
  if (!root || !root.isDirectory()) {
    return 0;
  }
  root.rewindDirectory();

  int count = 0;
  char name[128];
  while (true) {
    FsFile entry;
    FileGuard entryGuard(entry);
    entry = root.openNextFile();
    if (!entry) {
      break;
    }
    if (!entry.isDirectory()) {
      continue;
    }
    entry.getName(name, sizeof(name));
    const std::string statsPath = std::string(parentDir) + "/" + name + "/statistics.bin";
    if (!SdMan.exists(statsPath.c_str())) {
      continue;
    }
    if (fn(std::string(name))) {
      count++;
    }
  }
  return count;
}

}  // namespace

/**
 * Writes every book's stats to a single human-editable JSON file at `<backupRoot>/reading_stats.json`.
 * Reading time is exposed as whole seconds ("totalReadingTimeSeconds") for easy hand-editing; the
 * "type" + "id" fields identify the book's cache dir and must be kept intact for restore to match.
 *
 * @param backupRoot Destination root (e.g. "/.backups/reading_stats")
 * @return Number of books written
 */
int backupAllBookStats(const char* backupRoot) {
  if (!backupRoot) return 0;
  SdMan.mkdir(backupRoot);

  JsonDocument doc;
  JsonObject rootObj = doc.to<JsonObject>();
  rootObj["version"] = 1;
  rootObj["note"] =
      "Edit totalReadingTimeSeconds (or other fields) then use Settings > Actions > Restore Reading "
      "Stats. Keep each book's type and id unchanged so it maps back to the right book.";
  JsonArray books = rootObj["books"].to<JsonArray>();

  int count = 0;
  for (const auto& root : kStatsCacheRoots) {
    forEachStatsSubdir(root.cacheRoot, [&](const std::string& hash) -> bool {
      const std::string cacheDir = std::string(root.cacheRoot) + "/" + hash;
      BookReadingStats s;
      if (!loadBookStats(cacheDir.c_str(), s)) {
        return false;
      }
      JsonObject o = books.add<JsonObject>();
      o["type"] = root.typeTag;
      o["id"] = hash;
      o["title"] = s.title;
      o["author"] = s.author;
      o["totalReadingTimeSeconds"] = s.totalReadingTimeMs / 1000;
      o["totalPagesRead"] = s.totalPagesRead;
      o["totalChaptersRead"] = s.totalChaptersRead;
      o["sessionCount"] = s.sessionCount;
      o["progressPercent"] = s.progressPercent;
      o["lastSpineIndex"] = s.lastSpineIndex;
      o["lastPageNumber"] = s.lastPageNumber;
      o["avgPageTimeMs"] = s.avgPageTimeMs;
      o["lastReadTimeMs"] = s.lastReadTimeMs;
      count++;
      return true;
    });
  }

  const std::string jsonPath = std::string(backupRoot) + "/" + kReadingStatsJson;
  FsFile f = SdMan.open(jsonPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!f) {
    Serial.printf("[%lu] [STBK] Failed to open %s for write\n", millis(), jsonPath.c_str());
    return 0;
  }
  serializeJsonPretty(doc, f);
  f.close();

  Serial.printf("[%lu] [STBK] Backup complete: %d book(s) -> %s\n", millis(), count, jsonPath.c_str());
  return count;
}

/**
 * Restores stats from `<backupRoot>/reading_stats.json` into the per-book cache dirs, replacing
 * current values. Missing JSON fields keep whatever the book already had; the cache dir is created
 * if absent so stats survive a firmware flash (hash-matched). Reading time is read in seconds.
 *
 * @param backupRoot Source root (e.g. "/.backups/reading_stats")
 * @return Number of books restored
 */
int restoreAllBookStats(const char* backupRoot) {
  if (!backupRoot) return 0;

  const std::string jsonPath = std::string(backupRoot) + "/" + kReadingStatsJson;
  FsFile f = SdMan.open(jsonPath.c_str(), O_READ);
  if (!f) {
    Serial.printf("[%lu] [STBK] No backup file at %s\n", millis(), jsonPath.c_str());
    return 0;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err || !doc["books"].is<JsonArray>()) {
    Serial.printf("[%lu] [STBK] Invalid JSON in %s (%s)\n", millis(), jsonPath.c_str(), err.c_str());
    return 0;
  }

  int restored = 0;
  for (JsonObject o : doc["books"].as<JsonArray>()) {
    const char* type = o["type"] | "";
    const char* id = o["id"] | "";
    const char* cacheRoot = cacheRootForType(type);
    if (cacheRoot == nullptr || id[0] == '\0') {
      continue;
    }

    const std::string cacheDir = std::string(cacheRoot) + "/" + id;

    // Start from whatever the book already has so unspecified JSON fields are preserved.
    BookReadingStats s;
    loadBookStats(cacheDir.c_str(), s);
    s.path = cacheDir;
    s.title = o["title"] | s.title.c_str();
    s.author = o["author"] | s.author.c_str();
    s.totalReadingTimeMs = (o["totalReadingTimeSeconds"] | (s.totalReadingTimeMs / 1000)) * 1000;
    s.totalPagesRead = o["totalPagesRead"] | s.totalPagesRead;
    s.totalChaptersRead = o["totalChaptersRead"] | s.totalChaptersRead;
    s.sessionCount = o["sessionCount"] | s.sessionCount;
    s.progressPercent = o["progressPercent"] | s.progressPercent;
    s.lastSpineIndex = o["lastSpineIndex"] | s.lastSpineIndex;
    s.lastPageNumber = o["lastPageNumber"] | s.lastPageNumber;
    s.avgPageTimeMs = o["avgPageTimeMs"] | s.avgPageTimeMs;
    s.lastReadTimeMs = o["lastReadTimeMs"] | s.lastReadTimeMs;

    SdMan.mkdir(cacheDir.c_str());
    saveBookStats(cacheDir.c_str(), s);
    restored++;
    Serial.printf("[%lu] [STBK] Restored %s (%u s)\n", millis(), cacheDir.c_str(), s.totalReadingTimeMs / 1000);
  }

  if (restored > 0) {
    saveGlobalStats(generateGlobalStats());
  }
  Serial.printf("[%lu] [STBK] Restore complete: %d book(s)\n", millis(), restored);
  return restored;
}
