/**
 * @file LocalServer.cpp
 * @brief Definitions for LocalServer.
 */

#include "LocalServer.h"

#include <ArduinoJson.h>
#ifndef INX_SIMULATOR_WEB_ONLY
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <FsHelpers.h>
#include <HalGPIO.h>
#endif
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#ifndef SIMULATOR
#include <esp_ota_ops.h>
#include <esp_system.h>
#include "OtaUpdater.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

#include "../state/SystemSetting.h"
#include "state/Session.h"
#include "state/SleepImageSelection.h"
#ifndef INX_SIMULATOR_WEB_ONLY
#include "activity/reader/Epub/EpubActivity.h"
#include "activity/reader/Epub/EpubAnnotations.h"
#endif
#include "html/EpubPageHtml.generated.h"
#include "html/EpubPageJs.generated.h"
#include "html/ExportPageHtml.generated.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FilesPageJs.generated.h"
#include "html/FontManagerPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/InxFontPackJs.generated.h"
#include "html/JsZipMinJs.generated.h"
#include "html/LibraryPageHtml.generated.h"
#include "html/LibraryPageJs.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/TagsPageHtml.generated.h"
#include "html/UpdatePageHtml.generated.h"
#ifndef INX_SIMULATOR_WEB_ONLY
#include "activity/settings/LibraryIndexer.h"
#include "state/BookState.h"
#include "state/BookTags.h"
#include "state/EpubNotesIndex.h"
#include "state/RecentBooks.h"
#include "state/Statistics.h"
#include "system/FontManager.h"
#ifndef SIMULATOR
#include "system/SleepWakeTraceStore.h"
#endif
#include "util/StringUtils.h"
#endif
#include "KOReaderCredentialStore.h"
#include "state/NetworkCredential.h"
#ifndef INX_SIMULATOR_WEB_ONLY
#include "state/OpdsServerStore.h"
#endif

namespace {

const char* HIDDEN_ITEMS[] = {"System Volume Information", ".metadata"};
constexpr size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;
constexpr size_t MIN_FIRMWARE_SIZE = 64 * 1024;
constexpr uint8_t ESP_IMAGE_MAGIC = 0xE9;
constexpr uint16_t ESP32_C3_CHIP_ID = 5;

#ifndef SIMULATOR
const char* otaErrorMessage(const OtaUpdater::OtaUpdaterError error) {
  switch (error) {
    case OtaUpdater::OK:
      return "";
    case OtaUpdater::NO_UPDATE:
      return "The latest release does not contain a compatible firmware image";
    case OtaUpdater::HTTP_ERROR:
      return "Could not reach GitHub or download the firmware";
    case OtaUpdater::JSON_PARSE_ERROR:
      return "GitHub returned release information the reader could not understand";
    case OtaUpdater::UPDATE_OLDER_ERROR:
      return "The selected release is not newer than the installed firmware";
    case OtaUpdater::OOM_ERROR:
      return "The reader does not have enough free memory to check for this update";
    case OtaUpdater::INTERNAL_UPDATE_ERROR:
    default:
      return "The firmware update process could not be completed";
  }
}
#endif

LocalServer* wsInstance = nullptr;

volatile bool webLibraryIndexing = false;
volatile int webLibraryIndexCurrent = 0;
volatile int webLibraryIndexTotal = 0;
char webLibraryIndexPath[128] = "";

FsFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

void copySettingString(char* dest, size_t destSize, const char* value) {
  if (destSize == 0) {
    return;
  }
  if (value == nullptr) {
    value = "";
  }
  strncpy(dest, value, destSize - 1);
  dest[destSize - 1] = '\0';
}

void clearEpubCacheIfNeeded(const String& filePath) {
#ifndef INX_SIMULATOR_WEB_ONLY
  if (StringUtils::checkFileExtension(filePath, ".epub")) {
    Epub(filePath.c_str(), "/.metadata").clearCache();
    Serial.printf("[%lu] [WEB] Cleared epub cache for: %s\n", millis(), filePath.c_str());
  }
#else
  (void)filePath;
#endif
}

bool clockSettingsAvailable() {
#ifndef INX_SIMULATOR_WEB_ONLY
  return gpio.deviceIsX3();
#else
  return false;
#endif
}

#ifndef INX_SIMULATOR_WEB_ONLY
const char* sleepWakeTraceEventName(const HalGPIO::SleepWakeTraceEvent event) {
  switch (event) {
    case HalGPIO::SleepWakeTraceEvent::SleepPlan:
      return "sleep_plan";
    case HalGPIO::SleepWakeTraceEvent::ArmResult:
      return "arm_result";
    case HalGPIO::SleepWakeTraceEvent::SleepEnter:
      return "sleep_enter";
    case HalGPIO::SleepWakeTraceEvent::WakeStub:
      return "wake_stub";
    case HalGPIO::SleepWakeTraceEvent::EarlyWake:
      return "early_wake";
    case HalGPIO::SleepWakeTraceEvent::DeadlineCheck:
      return "deadline_check";
    case HalGPIO::SleepWakeTraceEvent::PowerSample:
      return "power_sample";
    case HalGPIO::SleepWakeTraceEvent::ClassifiedTimer:
      return "classified_timer";
    case HalGPIO::SleepWakeTraceEvent::PowerAction:
      return "power_action";
    case HalGPIO::SleepWakeTraceEvent::SetupWake:
      return "setup_wake";
    case HalGPIO::SleepWakeTraceEvent::ImageResult:
      return "image_result";
    case HalGPIO::SleepWakeTraceEvent::TimerBranch:
      return "timer_branch";
    case HalGPIO::SleepWakeTraceEvent::DoublePressBranch:
      return "double_press_branch";
    case HalGPIO::SleepWakeTraceEvent::ShortPressBranch:
      return "short_press_branch";
    case HalGPIO::SleepWakeTraceEvent::UiBranch:
      return "ui_branch";
    case HalGPIO::SleepWakeTraceEvent::SleepStage:
      return "sleep_stage";
    case HalGPIO::SleepWakeTraceEvent::PowerGesture:
      return "power_gesture";
  }
  return "unknown";
}
#endif

struct WallpaperInfo {
  String path;
  String label;
};

bool stringEndsWith(const std::string& value, const char* suffix) {
  const size_t suffixLength = strlen(suffix);
  return value.size() >= suffixLength && value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
}

bool hasSupportedWallpaperExtension(const String& value) {
  std::string lower = value.c_str();
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return stringEndsWith(lower, ".bmp") || stringEndsWith(lower, ".jpg") || stringEndsWith(lower, ".jpeg");
}

String normalizeWallpaperPath(String path) {
  path.trim();
  if (path.isEmpty()) {
    return "";
  }
  if (!path.startsWith("/")) {
    path = "/sleep/" + path;
  }
  return path;
}

bool isWallpaperPathAllowed(const String& path) {
  if (path.isEmpty() || !path.startsWith("/") || path.indexOf("..") >= 0 || path.indexOf('\\') >= 0 ||
      path.indexOf(':') >= 0 || !hasSupportedWallpaperExtension(path)) {
    return false;
  }
  return path == "/sleep.bmp" || path == "/sleep.jpg" || path == "/sleep.jpeg" || path.startsWith("/sleep/") ||
         path.startsWith("/Wallpapers/");
}

String wallpaperMimeType(const String& path) {
  std::string lower = path.c_str();
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (stringEndsWith(lower, ".jpg") || stringEndsWith(lower, ".jpeg")) {
    return "image/jpeg";
  }
  if (stringEndsWith(lower, ".bmp")) {
    return "image/bmp";
  }
  return "application/octet-stream";
}

bool wallpaperMatchesCurrentSelection(const String& path) {
  const String current = SETTINGS.sleepCustomBmp;
  if (current.isEmpty()) {
    return false;
  }
  if (current == path) {
    return true;
  }
  if (path.startsWith("/sleep/")) {
    return current == path.substring(7);
  }
  return false;
}

void appendWallpaperFolder(std::vector<WallpaperInfo>& items, const char* folder) {
  auto dir = SdMan.open(folder);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return;
  }

  char name[256];
  while (auto file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const String filename = name;
    if (!file.isDirectory() && !filename.isEmpty() && !filename.startsWith(".") &&
        hasSupportedWallpaperExtension(filename)) {
      WallpaperInfo info;
      info.path = String(folder) + "/" + filename;
      info.label = strcmp(folder, "/Wallpapers") == 0 ? String("Wallpapers/") + filename : filename;
      items.push_back(info);
    }
    file.close();
  }
  dir.close();
}

std::vector<WallpaperInfo> collectWallpapers() {
  std::vector<WallpaperInfo> items;
  appendWallpaperFolder(items, "/sleep");
  appendWallpaperFolder(items, "/Wallpapers");
  std::sort(items.begin(), items.end(),
            [](const WallpaperInfo& a, const WallpaperInfo& b) { return a.label < b.label; });

  if (SdMan.exists("/sleep.bmp")) {
    items.push_back({"/sleep.bmp", "sleep.bmp (SD root)"});
  }
  if (SdMan.exists("/sleep.jpg")) {
    items.push_back({"/sleep.jpg", "sleep.jpg (SD root)"});
  }
  if (SdMan.exists("/sleep.jpeg")) {
    items.push_back({"/sleep.jpeg", "sleep.jpeg (SD root)"});
  }
  return items;
}

int enabledWallpaperCount(const std::vector<WallpaperInfo>& items) {
  int count = 0;
  for (const auto& item : items) {
    if (isSleepImageShuffleEnabled(item.path.c_str())) {
      count++;
    }
  }
  return count;
}

#ifndef INX_SIMULATOR_WEB_ONLY
struct IndexedBookInfo {
  String path;
  String title;
  String folder;
  String tag;
};

std::string lowerAscii(const String& value) {
  std::string lowered = value.c_str();
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lowered;
}

String jsonEscape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s.charAt(i);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

bool readExactString(FsFile& file, size_t len, String& out) {
  out = "";
  if (len == 0) {
    return true;
  }
  std::vector<char> buf(len + 1, 0);
  if (file.read(buf.data(), len) != static_cast<int>(len)) {
    return false;
  }
  out = String(buf.data());
  return true;
}

String indexedFolderName(const String& path) {
  if (path == "/" || path.length() == 0) {
    return "Library";
  }
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash < 0) {
    return path;
  }
  String name = path.substring(lastSlash + 1);
  return name.length() ? name : "Library";
}

bool readIndexedBook(FsFile& idxFile, IndexedBookInfo& out) {
  uint16_t pLen = 0;
  if (idxFile.read(&pLen, sizeof(pLen)) != sizeof(pLen)) return false;
  if (!readExactString(idxFile, pLen, out.path)) return false;

  uint8_t nLen = 0;
  if (idxFile.read(&nLen, sizeof(nLen)) != sizeof(nLen)) return false;
  idxFile.seek(idxFile.position() + nLen);

  uint8_t dLen = 0;
  if (idxFile.read(&dLen, sizeof(dLen)) != sizeof(dLen)) return false;
  if (!readExactString(idxFile, dLen, out.title)) return false;

  uint8_t fLen = 0;
  if (idxFile.read(&fLen, sizeof(fLen)) != sizeof(fLen)) return false;
  if (!readExactString(idxFile, fLen, out.folder)) return false;
  if (out.folder.length() == 0) {
    int slash = out.path.lastIndexOf('/');
    out.folder = slash <= 0 ? "Library" : indexedFolderName(out.path.substring(0, slash));
  }
  return true;
}

void skipIndexedDirectory(FsFile& idxFile) {
  uint16_t pathLen = 0;
  if (idxFile.read(&pathLen, sizeof(pathLen)) != sizeof(pathLen)) return;
  idxFile.seek(idxFile.position() + pathLen);
  uint16_t entryCount = 0;
  idxFile.read(&entryCount, sizeof(entryCount));
}

bool loadIndexedBooksWithTags(std::vector<IndexedBookInfo>& books) {
  books.clear();
  FsFile idxFile = SdMan.open("/.metadata/library/library.idx", O_READ);
  if (!idxFile) {
    return false;
  }

  char magic[4] = {};
  uint8_t version = 0;
  if (idxFile.read(magic, 4) != 4 || memcmp(magic, "LIBX", 4) != 0 || idxFile.read(&version, 1) != 1) {
    idxFile.close();
    return false;
  }

  std::vector<BookTags::Entry> tags;
  BookTags::load(tags);

  while (idxFile.available()) {
    uint8_t marker = 0;
    if (idxFile.read(&marker, 1) != 1) break;
    if (marker == 0x01) {
      IndexedBookInfo book;
      if (!readIndexedBook(idxFile, book)) break;
      const std::string tag = BookTags::find(tags, book.path.c_str());
      book.tag = tag.c_str();
      books.push_back(book);
      if ((books.size() % 64u) == 0u) {
        yield();
      }
    } else if (marker == 0xFF) {
      skipIndexedDirectory(idxFile);
    }
  }

  idxFile.close();
  return true;
}

std::string epubCachePathForBookPath(const std::string& bookPath) {
  return "/.metadata/epub/" + std::to_string(std::hash<std::string>{}(bookPath));
}

std::string cachePathForIndexedBook(const String& bookPath) {
  std::string lower = bookPath.c_str();
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const std::string path = bookPath.c_str();
  if (stringEndsWith(lower, ".xtc") || stringEndsWith(lower, ".xtch")) {
    return "/.metadata/xtc/" + std::to_string(std::hash<std::string>{}(path));
  }
  return epubCachePathForBookPath(path);
}

String bookFileType(const String& bookPath) {
  std::string lower = bookPath.c_str();
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (stringEndsWith(lower, ".epub")) return "EPUB";
  if (stringEndsWith(lower, ".xtc") || stringEndsWith(lower, ".xtch")) return "XTC";
  if (stringEndsWith(lower, ".md")) return "Markdown";
  if (stringEndsWith(lower, ".txt")) return "Text";
  return "Book";
}

size_t fileSizeForPath(const String& path) {
  FsFile file = SdMan.open(path.c_str(), O_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    return 0;
  }
  const size_t size = file.fileSize();
  file.close();
  return size;
}

String coverUrlForCachePath(const std::string& cachePath) {
  const std::string coverJpeg = cachePath + "/cover.jpg";
  const std::string thumbJpeg = cachePath + "/thumb.jpg";
  const std::string coverBmp = cachePath + "/cover.bmp";
  const std::string thumbBmp = cachePath + "/thumb.bmp";
  std::string coverPath;
  if (SdMan.exists(coverJpeg.c_str())) {
    coverPath = coverJpeg;
  } else if (SdMan.exists(thumbJpeg.c_str())) {
    coverPath = thumbJpeg;
  } else if (SdMan.exists(coverBmp.c_str())) {
    coverPath = coverBmp;
  } else if (SdMan.exists(thumbBmp.c_str())) {
    coverPath = thumbBmp;
  }
  if (coverPath.empty()) {
    return "";
  }
  String url = "/download?path=";
  url += coverPath.c_str();
  url += "&inline=1";
  return url;
}

std::vector<std::string> epubCacheDirs() {
  std::vector<std::string> out;
  FsFile root = SdMan.open("/.metadata/epub");
  if (!root || !root.isDirectory()) {
    if (root) {
      root.close();
    }
    return out;
  }
  char name[96];
  for (FsFile f = root.openNextFile(); f; f = root.openNextFile()) {
    if (f.isDirectory()) {
      f.getName(name, sizeof(name));
      out.push_back(std::string("/.metadata/epub/") + name);
    }
    f.close();
  }
  root.close();
  return out;
}

struct ExportBookInfo {
  std::string title;
  std::string author;
  std::string coverPath;
};

ExportBookInfo exportBookInfoForCachePath(const std::string& cachePath) {
  ExportBookInfo info;
  BookMetadataCache metadata(cachePath);
  if (metadata.load()) {
    info.title = metadata.coreMetadata.title;
    info.author = metadata.coreMetadata.author;
  }

  RECENT_BOOKS.loadFromFile();
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    const std::string bookCache = book.cachePath.empty() ? epubCachePathForBookPath(book.path) : book.cachePath;
    if (bookCache == cachePath) {
      if (info.title.empty()) {
        info.title = book.title;
      }
      if (info.author.empty()) {
        info.author = book.author;
      }
      if (!info.title.empty()) {
        break;
      }
      const size_t slash = book.path.find_last_of('/');
      info.title = slash == std::string::npos ? book.path : book.path.substr(slash + 1);
      break;
    }
  }

  if (info.title.empty()) {
    const size_t slash = cachePath.find_last_of('/');
    info.title = slash == std::string::npos ? cachePath : cachePath.substr(slash + 1);
  }
  const size_t dot = info.title.find_last_of('.');
  if (dot != std::string::npos) {
    info.title.resize(dot);
  }
  const std::string coverJpeg = cachePath + "/cover.jpg";
  const std::string thumbJpeg = cachePath + "/thumb.jpg";
  const std::string coverBmp = cachePath + "/cover.bmp";
  if (SdMan.exists(coverJpeg.c_str())) {
    info.coverPath = coverJpeg;
  } else if (SdMan.exists(thumbJpeg.c_str())) {
    info.coverPath = thumbJpeg;
  } else if (SdMan.exists(coverBmp.c_str())) {
    info.coverPath = coverBmp;
  }
  return info;
}

std::string bookmarkPreviewText(const std::string& cachePath, const int spine, const int page) {
  std::unique_ptr<Page> cachedPage = Section::loadCachedPage(cachePath, spine, page);
  if (!cachedPage) {
    return "";
  }
  return cachedPage->extractPlainText(1600);
}

void writeFileString(FsFile& file, const String& value) {
  file.write(reinterpret_cast<const uint8_t*>(value.c_str()), value.length());
}

void writeFileString(FsFile& file, const char* value) {
  file.write(reinterpret_cast<const uint8_t*>(value), strlen(value));
}

void writeExportNoteItem(FsFile& file, bool& first, int& total, const char* type, const ExportBookInfo& book,
                         const std::string& chapter, const int spine, const int page, const int pageCount,
                         const uint32_t timestamp, const std::string& text, const std::string& pageText = "") {
  if (!first) {
    writeFileString(file, ",");
  }
  first = false;
  String row = "{\"type\":\"";
  row += type;
  row += "\",\"book\":\"";
  row += jsonEscape(book.title.c_str());
  row += "\",\"author\":\"";
  row += jsonEscape(book.author.c_str());
  row += "\",\"coverUrl\":\"";
  if (!book.coverPath.empty()) {
    String coverUrl = "/download?path=";
    coverUrl += book.coverPath.c_str();
    coverUrl += "&inline=1";
    row += jsonEscape(coverUrl.c_str());
  }
  row += "\",\"chapter\":\"";
  row += jsonEscape(chapter.c_str());
  row += "\",\"spine\":";
  row += spine;
  row += ",\"page\":";
  row += page;
  row += ",\"pageCount\":";
  row += pageCount;
  row += ",\"timestamp\":";
  row += timestamp;
  row += ",\"text\":\"";
  row += jsonEscape(text.c_str());
  if (!pageText.empty()) {
    row += "\",\"pageText\":\"";
    row += jsonEscape(pageText.c_str());
  }
  row += "\"}";
  writeFileString(file, row);
  ++total;
}

bool buildExportNotesIndex() {
  SdMan.mkdir("/.metadata");
  SdMan.mkdir("/.metadata/epub");

  FsFile index;
  if (!SdMan.openFileForWrite("EXP", EpubNotesIndex::kPath, index)) {
    return false;
  }

  const std::vector<std::string> caches = epubCacheDirs();
  std::set<std::string> annotationKeys;
  bool first = true;
  int total = 0;
  String header = "{\"ok\":true,\"version\":";
  header += EpubNotesIndex::kVersion;
  header += ",\"items\":[";
  writeFileString(index, header);

  for (const std::string& cachePath : caches) {
    const ExportBookInfo book = exportBookInfoForCachePath(cachePath);
    const std::string bookmarksPath = cachePath + "/bookmarks.bin";
    FsFile f;
    if (SdMan.openFileForRead("EXP", bookmarksPath, f)) {
      const uint32_t fileSize = f.fileSize();
      const int count = fileSize / sizeof(EpubActivity::Bookmark);
      for (int i = 0; i < count && i < 200; ++i) {
        EpubActivity::Bookmark b{};
        if (f.read(&b, sizeof(b)) != sizeof(b)) {
          break;
        }
        if (b.isValid()) {
          const std::string text = bookmarkPreviewText(cachePath, b.spineIndex, b.pageNumber);
          writeExportNoteItem(index, first, total, "bookmark", book, b.chapterTitle, b.spineIndex, b.pageNumber,
                              std::max<int>(1, b.pageCount), b.timestamp, text);
        }
      }
      f.close();
    }

    const std::string annDir = cachePath + "/" + EpubAnnotations::kSubdir;
    if (SdMan.exists(annDir.c_str())) {
      const std::vector<String> files = SdMan.listFiles(annDir.c_str());
      EpubAnnotations annotations;
      for (const String& file : files) {
        int spine = 0;
        int page = 0;
        if (std::sscanf(file.c_str(), "s_%d_p_%d.bin", &spine, &page) != 2) {
          continue;
        }
        annotations.ensurePageLoaded(cachePath, spine, page);
        for (const EpubAnnotationRecord& rec : annotations.records()) {
          const std::string key = cachePath + "|" + std::to_string(rec.timestamp) + "|" +
                                  std::to_string(rec.startSpine) + "|" + std::to_string(rec.startPage) + "|" +
                                  std::to_string(rec.endSpine) + "|" + std::to_string(rec.endPage) + "|" + rec.text;
          if (!annotationKeys.insert(key).second) {
            continue;
          }
          const int startPage = rec.startPage == EpubAnnotations::kWildcard ? page : rec.startPage;
          const int startSpine = rec.startSpine == EpubAnnotations::kWildcard ? spine : rec.startSpine;
          const std::string pageText = bookmarkPreviewText(cachePath, startSpine, startPage);
          writeExportNoteItem(index, first, total, "annotation", book, "Highlight", startSpine, startPage, 0,
                              rec.timestamp, rec.text, pageText);
        }
        yield();
      }
    }
    yield();
  }

  writeFileString(index, "],\"count\":");
  writeFileString(index, String(total));
  writeFileString(index, "}");
  index.close();
  return true;
}

bool exportNotesIndexIsCurrent() {
  FsFile index;
  if (!SdMan.openFileForRead("EXP", EpubNotesIndex::kPath, index)) {
    return false;
  }
  char buf[96] = {};
  const int n = index.read(buf, sizeof(buf) - 1);
  index.close();
  if (n <= 0) {
    return false;
  }
  String marker = "\"version\":";
  marker += EpubNotesIndex::kVersion;
  return strstr(buf, marker.c_str()) != nullptr;
}

void webLibraryIndexTask(void*) {
  webLibraryIndexCurrent = 0;
  webLibraryIndexTotal = 0;
  webLibraryIndexPath[0] = '\0';

  FsFile root = SdMan.open("/");
  if (root) {
    webLibraryIndexTotal = LibraryIndexer::countBooks(root);
    root.close();
  }

  vTaskDelay(pdMS_TO_TICKS(10));

  LibraryIndexer::indexAll([](int current, int total, const char* path) {
    webLibraryIndexCurrent = current;
    webLibraryIndexTotal = total;
    if (path) {
      strlcpy(webLibraryIndexPath, path, sizeof(webLibraryIndexPath));
    }
    if (current % 10 == 0) {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  });

  SETTINGS.useLibraryIndex = 1;
  SETTINGS.saveToFile();
  webLibraryIndexing = false;
  vTaskDelete(nullptr);
}
#endif
}  // namespace

LocalServer::LocalServer() {
#ifndef SIMULATOR
  githubUpdater.reset(new OtaUpdater());
#endif
}

LocalServer::~LocalServer() { stop(); }

void LocalServer::begin() {
  if (running) {
    Serial.printf("[%lu] [WEB] Web server already running\n", millis());
    return;
  }

  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);

  if (!isStaConnected && !isInApMode) {
    Serial.printf("[%lu] [WEB] Cannot start webserver - no valid network (mode=%d, status=%d)\n", millis(), wifiMode,
                  WiFi.status());
    return;
  }

  apMode = isInApMode;

  Serial.printf("[%lu] [WEB] [MEM] Free heap before begin: %d bytes\n", millis(), ESP.getFreeHeap());
  Serial.printf("[%lu] [WEB] Network mode: %s\n", millis(), apMode ? "AP" : "STA");

  Serial.printf("[%lu] [WEB] Creating web server on port %d...\n", millis(), port);
  server.reset(new WebServer(port));

  WiFi.setSleep(false);

  Serial.printf("[%lu] [WEB] [MEM] Free heap after WebServer allocation: %d bytes\n", millis(), ESP.getFreeHeap());

  if (!server) {
    Serial.printf("[%lu] [WEB] Failed to create WebServer!\n", millis());
    return;
  }

  Serial.printf("[%lu] [WEB] Setting up routes...\n", millis());
  char token[33] = {};
#ifndef SIMULATOR
  snprintf(token, sizeof(token), "%08lx%08lx%08lx%08lx", static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
#else
  snprintf(token, sizeof(token), "simulator-update-token");
#endif
  firmwareUploadToken = token;
  resetFirmwareUpload();

  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/library", HTTP_GET, [this] { handleLibraryPage(); });
  server->on("/epub", HTTP_GET, [this] { handleEpubPage(); });
  server->on("/export", HTTP_GET, [this] { handleExportPage(); });
  server->on("/font-manager", HTTP_GET, [this] { handleFontManagerPage(); });
  server->on("/tags", HTTP_GET, [this] { handleTagsPage(); });
  server->on("/js/inx_font_pack.js", HTTP_GET, [this] { handleInxFontPackJs(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJsZipMinJs(); });
  server->on("/js/epub_page.js", HTTP_GET, [this] { handleEpubPageJs(); });
  server->on("/js/files_page.js", HTTP_GET, [this] { handleFilesPageJs(); });
  server->on("/js/library_page.js", HTTP_GET, [this] { handleLibraryPageJs(); });
  server->on("/update", HTTP_GET, [this] { handleUpdatePage(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/api/library", HTTP_GET, [this] { handleLibraryData(); });
  server->on("/api/export-notes", HTTP_GET, [this] { handleExportNotesData(); });
  server->on("/api/book-tags", HTTP_GET, [this] { handleBookTagsGet(); });
  server->on("/api/book-tags", HTTP_POST, [this] { handleBookTagsPost(); });
  server->on("/api/library-index/refresh", HTTP_POST, [this] { handleLibraryIndexRefresh(); });
  server->on("/api/library-index/status", HTTP_GET, [this] { handleLibraryIndexStatus(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  server->on("/upload", HTTP_POST, [this] { handleUploadPost(); }, [this] { handleUpload(); });
  server->on("/api/update/status", HTTP_GET, [this] { handleFirmwareStatus(); });
  server->on("/api/update/github", HTTP_GET, [this] { handleGithubFirmwareCheck(); });
  server->on("/api/update/github/install", HTTP_POST, [this] { handleGithubFirmwareInstall(); });
  server->on("/api/update/github/install/status", HTTP_GET,
             [this] { handleGithubFirmwareInstallStatus(); });
  server->on(
      "/api/update/upload", HTTP_POST, [this] { handleFirmwareUploadPost(); }, [this] { handleFirmwareUpload(); });

  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleSettingsGet(); });
  server->on("/api/settings", HTTP_POST, [this] { handleSettingsUpdate(); });
  server->on("/api/sleep-wake-trace", HTTP_GET, [this] { handleSleepWakeTraceGet(); });
  server->on("/api/sleep-wake-trace", HTTP_DELETE, [this] { handleSleepWakeTraceClear(); });
  server->on("/api/wallpapers", HTTP_GET, [this] { handleWallpapersGet(); });
  server->on("/api/wallpapers/shuffle", HTTP_POST, [this] { handleWallpaperShufflePost(); });
  server->on("/wallpaper-preview", HTTP_GET, [this] { handleWallpaperImageGet(); });

  server->on("/api/wifi", HTTP_GET, [this] { handleWifiGet(); });
  server->on("/api/wifi", HTTP_POST, [this] { handleWifiPost(); });
  server->on("/api/wifi/*", HTTP_DELETE, [this] { handleWifiDelete(); });
  server->on("/api/koreader", HTTP_GET, [this] { handleKOReaderGet(); });
  server->on("/api/koreader", HTTP_POST, [this] { handleKOReaderPost(); });

#ifndef INX_SIMULATOR_WEB_ONLY
  server->on("/api/opds", HTTP_GET, [this] { handleOpdsGet(); });
  server->on("/api/opds", HTTP_POST, [this] { handleOpdsPost(); });
  server->on("/api/opds/*", HTTP_DELETE, [this] { handleOpdsDelete(); });
#endif

  server->on("/api/fonts/rescan", HTTP_POST, [this] { handleFontsRescan(); });

  server->onNotFound([this] { handleNotFound(); });
  Serial.printf("[%lu] [WEB] [MEM] Free heap after route setup: %d bytes\n", millis(), ESP.getFreeHeap());
  Serial.printf("✓ jszip.min.js from firmware flash (%u bytes)\n", static_cast<unsigned>(sizeof(JSZIP_MIN_JS) - 1));
  Serial.printf("✓ epub_page.js from firmware flash (%u bytes)\n", static_cast<unsigned>(sizeof(EPUB_PAGE_JS) - 1));
  Serial.printf("✓ files_page.js from firmware flash (%u bytes)\n", static_cast<unsigned>(sizeof(FILES_PAGE_JS) - 1));
  Serial.printf("✓ inx_font_pack.js from firmware flash (%u bytes)\n",
                static_cast<unsigned>(sizeof(INX_FONT_PACK_JS) - 1));

  server->begin();

  Serial.printf("[%lu] [WEB] Starting WebSocket server on port %d...\n", millis(), wsPort);
  wsServer.reset(new WebSocketsServer(wsPort));
  wsInstance = const_cast<LocalServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);
  Serial.printf("[%lu] [WEB] WebSocket server started\n", millis());

  udpActive = udp.begin(LOCAL_UDP_PORT);
  Serial.printf("[%lu] [WEB] Discovery UDP %s on port %d\n", millis(), udpActive ? "enabled" : "failed",
                LOCAL_UDP_PORT);

  running = true;

  Serial.printf("[%lu] [WEB] Web server started on port %d\n", millis(), port);

  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  Serial.printf("[%lu] [WEB] Access at http://%s/\n", millis(), ipAddr.c_str());
  Serial.printf("[%lu] [WEB] WebSocket at ws://%s:%d/\n", millis(), ipAddr.c_str(), wsPort);
  Serial.printf("[%lu] [WEB] [MEM] Free heap after server.begin(): %d bytes\n", millis(), ESP.getFreeHeap());
}

void LocalServer::stop() {
#ifndef SIMULATOR
  if (githubInstallState.load(std::memory_order_acquire) == GithubInstallState::RUNNING) {
    Serial.printf("[%lu] [WEB] Waiting for active GitHub firmware install before stopping\n", millis());
    while (githubInstallState.load(std::memory_order_acquire) == GithubInstallState::RUNNING) {
      esp_task_wdt_reset();
      delay(25);
    }
  }
#endif

  if (!running || !server) {
    Serial.printf("[%lu] [WEB] stop() called but already stopped (running=%d, server=%p)\n", millis(), running,
                  server.get());
    return;
  }

  Serial.printf("[%lu] [WEB] STOP INITIATED - setting running=false first\n", millis());
  running = false;

  Serial.printf("[%lu] [WEB] [MEM] Free heap before stop: %d bytes\n", millis(), ESP.getFreeHeap());

  if (wsUploadInProgress && wsUploadFile) {
    wsUploadFile.close();
    wsUploadInProgress = false;
  }

  if (firmwareOtaActive) {
    abortFirmwareUpload("Update server stopped");
  }

  if (wsServer) {
    Serial.printf("[%lu] [WEB] Stopping WebSocket server...\n", millis());
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
    Serial.printf("[%lu] [WEB] WebSocket server stopped\n", millis());
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  delay(20);

  server->stop();
  Serial.printf("[%lu] [WEB] [MEM] Free heap after server->stop(): %d bytes\n", millis(), ESP.getFreeHeap());

  delay(10);

  server.reset();
  Serial.printf("[%lu] [WEB] Web server stopped and deleted\n", millis());
  Serial.printf("[%lu] [WEB] [MEM] Free heap after delete server: %d bytes\n", millis(), ESP.getFreeHeap());

  Serial.printf("[%lu] [WEB] [MEM] Free heap final: %d bytes\n", millis(), ESP.getFreeHeap());
}

void LocalServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  if (!running) {
    return;
  }

  if (!server) {
    Serial.printf("[%lu] [WEB] WARNING: handleClient called with null server!\n", millis());
    return;
  }

  if (millis() - lastDebugPrint > 10000) {
    Serial.printf("[%lu] [WEB] handleClient active, server running on port %d\n", millis(), port);
    lastDebugPrint = millis();
  }

  server->handleClient();

  if (firmwareRestartAt != 0 && static_cast<long>(millis() - firmwareRestartAt) >= 0) {
    firmwareRestartAt = 0;
    Serial.printf("[%lu] [WEB] Rebooting into installed firmware\n", millis());
#ifndef SIMULATOR
    delay(50);
    ESP.restart();
#endif
  }

  const unsigned long githubRestartAt = githubInstallRestartAt.load(std::memory_order_relaxed);
  if (githubRestartAt != 0 && static_cast<long>(millis() - githubRestartAt) >= 0) {
    githubInstallRestartAt.store(0, std::memory_order_relaxed);
    Serial.printf("[%lu] [WEB] Rebooting into GitHub-installed firmware\n", millis());
#ifndef SIMULATOR
    delay(50);
    ESP.restart();
#endif
  }

  if (wsServer) {
    wsServer->loop();
  }

  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

LocalServer::WsUploadStatus LocalServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

void LocalServer::handleRoot() const {
  server->send(200, "text/html", HomePageHtml);
  Serial.printf("[%lu] [WEB] Served root page\n", millis());
}

void LocalServer::handleUpdatePage() const {
  server->send(200, "text/html", UpdatePageHtml);
  Serial.printf("[%lu] [WEB] Served firmware update page\n", millis());
}

void LocalServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void LocalServer::handleStatus() const {
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = INX_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
#ifndef INX_SIMULATOR_WEB_ONLY
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";
  doc["displayWidth"] = gpio.deviceIsX3() ? 792 : 800;
  doc["displayHeight"] = gpio.deviceIsX3() ? 528 : 480;
  doc["screenWidth"] = gpio.deviceIsX3() ? 528 : 480;
  doc["screenHeight"] = gpio.deviceIsX3() ? 792 : 800;
#else
  doc["device"] = "Simulator";
  doc["displayWidth"] = 792;
  doc["displayHeight"] = 528;
  doc["screenWidth"] = 528;
  doc["screenHeight"] = 792;
#endif

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

const char* LocalServer::firmwareUploadStateName() const {
  switch (firmwareUploadState) {
    case FirmwareUploadState::RECEIVING:
      return "receiving";
    case FirmwareUploadState::READY_TO_REBOOT:
      return "ready_to_reboot";
    case FirmwareUploadState::FAILED:
      return "failed";
    case FirmwareUploadState::IDLE:
    default:
      return "idle";
  }
}

void LocalServer::resetFirmwareUpload() {
#ifndef SIMULATOR
  if (firmwareOtaActive) {
    esp_ota_abort(static_cast<esp_ota_handle_t>(firmwareOtaHandle));
  }
#endif
  firmwareUploadState = FirmwareUploadState::IDLE;
  firmwareExpectedSize = 0;
  firmwareReceivedSize = 0;
  firmwareOtaHandle = 0;
  firmwareOtaPartition = nullptr;
  firmwareOtaActive = false;
  firmwareUploadName.clear();
  firmwareUploadError.clear();
  firmwareUploadVersion.clear();
  firmwareRestartAt = 0;
}

void LocalServer::abortFirmwareUpload(const char* error) {
  const std::string safeError = error && error[0] ? error : "Firmware upload failed";
#ifndef SIMULATOR
  if (firmwareOtaActive) {
    esp_ota_abort(static_cast<esp_ota_handle_t>(firmwareOtaHandle));
  }
#endif
  firmwareOtaHandle = 0;
  firmwareOtaPartition = nullptr;
  firmwareOtaActive = false;
  firmwareUploadState = FirmwareUploadState::FAILED;
  firmwareUploadError = safeError;
  Serial.printf("[%lu] [WEB] [UPDATE] %s\n", millis(), firmwareUploadError.c_str());
}

void LocalServer::handleFirmwareStatus() const {
  JsonDocument doc;
  doc["currentVersion"] = INX_VERSION;
  doc["state"] = firmwareUploadStateName();
  doc["filename"] = firmwareUploadName;
  doc["received"] = firmwareReceivedSize;
  doc["total"] = firmwareExpectedSize;
  doc["candidateVersion"] = firmwareUploadVersion;
  doc["error"] = firmwareUploadError;
  doc["token"] = firmwareUploadToken;
  doc["rebootPending"] = firmwareRestartAt != 0;

#ifndef SIMULATOR
  const esp_partition_t* runningPartition = esp_ota_get_running_partition();
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  doc["activePartition"] = runningPartition ? runningPartition->label : "";
  doc["targetPartition"] = updatePartition ? updatePartition->label : "";
  doc["maxSize"] = updatePartition ? updatePartition->size : 0;

  esp_ota_img_states_t imageState = ESP_OTA_IMG_UNDEFINED;
  if (runningPartition && esp_ota_get_state_partition(runningPartition, &imageState) == ESP_OK) {
    doc["activeImageState"] = static_cast<int>(imageState);
  }
#else
  doc["activePartition"] = "sim";
  doc["targetPartition"] = "sim";
  doc["maxSize"] = 0;
#endif

  String json;
  serializeJson(doc, json);
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/json", json);
}

void LocalServer::handleGithubFirmwareCheck() {
  JsonDocument doc;
  doc["currentVersion"] = INX_VERSION;

#ifdef SIMULATOR
  doc["ok"] = false;
  doc["error"] = "GitHub firmware checks are unavailable in the simulator";
  String json;
  serializeJson(doc, json);
  server->send(501, "application/json", json);
#else
  if (githubInstallState.load(std::memory_order_acquire) == GithubInstallState::RUNNING ||
      githubInstallRestartAt.load(std::memory_order_relaxed) != 0) {
    doc["ok"] = false;
    doc["error"] = "A GitHub firmware installation is already in progress or restarting";
    String json;
    serializeJson(doc, json);
    server->send(409, "application/json", json);
    return;
  }
  if (!githubUpdater) githubUpdater.reset(new OtaUpdater());
  githubInstallState.store(GithubInstallState::IDLE, std::memory_order_release);
  githubInstallRestartAt.store(0, std::memory_order_relaxed);
  githubInstallVersion.clear();
  githubInstallError.clear();
  Serial.printf("[%lu] [WEB] [UPDATE] Checking GitHub for the latest firmware release\n", millis());
  const auto result = githubUpdater->checkForUpdate();
  doc["ok"] = result == OtaUpdater::OK;
  doc["latestVersion"] = githubUpdater->getLatestVersion();
  doc["newer"] = result == OtaUpdater::OK && githubUpdater->isUpdateNewer();
  doc["notes"] = githubUpdater->getReleaseNotes();
  doc["size"] = githubUpdater->getOtaSize();
  if (result != OtaUpdater::OK) doc["error"] = otaErrorMessage(result);

  String json;
  serializeJson(doc, json);
  server->sendHeader("Cache-Control", "no-store");
  server->send(result == OtaUpdater::OK ? 200 : 502, "application/json", json);
#endif
}

void LocalServer::handleGithubFirmwareInstall() {
  JsonDocument doc;

#ifdef SIMULATOR
  doc["ok"] = false;
  doc["error"] = "GitHub firmware installation is unavailable in the simulator";
  String json;
  serializeJson(doc, json);
  server->send(501, "application/json", json);
#else
  if (firmwareUploadToken != server->arg("token").c_str()) {
    doc["ok"] = false;
    doc["error"] = "Invalid update session token";
    String json;
    serializeJson(doc, json);
    server->send(403, "application/json", json);
    return;
  }
  if (server->arg("confirm") != "1") {
    doc["ok"] = false;
    doc["error"] = "Installation confirmation is required";
    String json;
    serializeJson(doc, json);
    server->send(400, "application/json", json);
    return;
  }
  const GithubInstallState installState = githubInstallState.load(std::memory_order_acquire);
  if (installState == GithubInstallState::RUNNING) {
    doc["ok"] = false;
    doc["error"] = "A GitHub firmware installation is already in progress";
    String json;
    serializeJson(doc, json);
    server->send(409, "application/json", json);
    return;
  }
  if (firmwareRestartAt != 0 || githubInstallRestartAt.load(std::memory_order_relaxed) != 0) {
    doc["ok"] = false;
    doc["error"] = "The reader is already restarting into an installed update";
    String json;
    serializeJson(doc, json);
    server->send(409, "application/json", json);
    return;
  }
  if (firmwareUploadState == FirmwareUploadState::RECEIVING ||
      firmwareUploadState == FirmwareUploadState::READY_TO_REBOOT) {
    doc["ok"] = false;
    doc["error"] = "A local firmware update is already in progress";
    String json;
    serializeJson(doc, json);
    server->send(409, "application/json", json);
    return;
  }
  if (!githubUpdater || !githubUpdater->isUpdateNewer()) {
    doc["ok"] = false;
    doc["error"] = "Check GitHub and confirm a newer release before installing";
    String json;
    serializeJson(doc, json);
    server->send(409, "application/json", json);
    return;
  }

  githubInstallVersion = githubUpdater->getLatestVersion();
  githubInstallError.clear();
  githubInstallState.store(GithubInstallState::RUNNING, std::memory_order_release);
  Serial.printf("[%lu] [WEB] [UPDATE] Starting background install of GitHub release %s\n", millis(),
                githubInstallVersion.c_str());

  const BaseType_t created =
      xTaskCreate(githubInstallTaskEntry, "WebGithubOta", 8192, this, 2, nullptr);
  if (created != pdPASS) {
    githubInstallError = "Could not start the background firmware installer";
    githubInstallState.store(GithubInstallState::FAILED, std::memory_order_release);
    doc["ok"] = false;
    doc["error"] = githubInstallError;
    String json;
    serializeJson(doc, json);
    server->send(500, "application/json", json);
    return;
  }

  doc["ok"] = true;
  doc["state"] = "running";
  doc["version"] = githubInstallVersion;
  doc["total"] = githubUpdater->getTotalSize();
  String json;
  serializeJson(doc, json);
  server->sendHeader("Cache-Control", "no-store");
  server->send(202, "application/json", json);
#endif
}

const char* LocalServer::githubInstallStateName() const {
  switch (githubInstallState.load(std::memory_order_acquire)) {
    case GithubInstallState::RUNNING:
      return "running";
    case GithubInstallState::SUCCEEDED:
      return "succeeded";
    case GithubInstallState::FAILED:
      return "failed";
    case GithubInstallState::IDLE:
    default:
      return "idle";
  }
}

void LocalServer::handleGithubFirmwareInstallStatus() const {
  JsonDocument doc;
  const GithubInstallState state = githubInstallState.load(std::memory_order_acquire);
  size_t processed = 0;
  size_t total = 0;
#ifndef SIMULATOR
  if (githubUpdater) {
    processed = githubUpdater->getProcessedSize();
    total = githubUpdater->getTotalSize();
  }
#endif
  const unsigned percent = total > 0 ? std::min<unsigned>(100, (processed * 100ULL) / total) : 0;

  doc["ok"] = state != GithubInstallState::FAILED;
  doc["state"] = githubInstallStateName();
  doc["version"] = githubInstallVersion;
  doc["processed"] = processed;
  doc["total"] = total;
  doc["percent"] = percent;
  doc["error"] = state == GithubInstallState::FAILED ? githubInstallError : "";
  doc["rebootPending"] = githubInstallRestartAt.load(std::memory_order_relaxed) != 0;

  String json;
  serializeJson(doc, json);
  server->sendHeader("Cache-Control", "no-store");
  server->send(200, "application/json", json);
}

#ifndef SIMULATOR
void LocalServer::githubInstallTaskEntry(void* context) {
  static_cast<LocalServer*>(context)->runGithubInstallTask();
}

void LocalServer::runGithubInstallTask() {
  const auto result = githubUpdater ? githubUpdater->installUpdate() : OtaUpdater::INTERNAL_UPDATE_ERROR;
  if (result == OtaUpdater::OK) {
    githubInstallRestartAt.store(millis() + 5000, std::memory_order_relaxed);
    githubInstallState.store(GithubInstallState::SUCCEEDED, std::memory_order_release);
    Serial.printf("[%lu] [WEB] [UPDATE] GitHub firmware validated; reboot scheduled\n", millis());
  } else {
    githubInstallError = otaErrorMessage(result);
    githubInstallState.store(GithubInstallState::FAILED, std::memory_order_release);
    Serial.printf("[%lu] [WEB] [UPDATE] GitHub install failed: %s\n", millis(), githubInstallError.c_str());
  }
  vTaskDelete(nullptr);
}
#endif

void LocalServer::handleFirmwareUpload() {
  HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    resetFirmwareUpload();
    firmwareUploadName = upload.filename.c_str();

    if (githubInstallState.load(std::memory_order_acquire) == GithubInstallState::RUNNING ||
        githubInstallRestartAt.load(std::memory_order_relaxed) != 0) {
      abortFirmwareUpload("A GitHub firmware installation is already in progress or restarting");
      return;
    }

    if (firmwareUploadToken != server->arg("token").c_str()) {
      abortFirmwareUpload("Invalid update session token");
      return;
    }

    std::string lowerName = upload.filename.c_str();
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!stringEndsWith(lowerName, ".bin")) {
      abortFirmwareUpload("Select an ESP32 firmware .bin file");
      return;
    }

    const unsigned long declaredSize = strtoul(server->arg("size").c_str(), nullptr, 10);
    firmwareExpectedSize = static_cast<size_t>(declaredSize);
    if (firmwareExpectedSize < MIN_FIRMWARE_SIZE) {
      abortFirmwareUpload("Firmware file is missing or too small");
      return;
    }

#ifdef SIMULATOR
    abortFirmwareUpload("Firmware installation is unavailable in the simulator");
    return;
#else
    const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
    if (updatePartition == nullptr) {
      abortFirmwareUpload("No inactive OTA partition is available");
      return;
    }
    if (firmwareExpectedSize > updatePartition->size) {
      abortFirmwareUpload("Firmware is larger than the inactive OTA partition");
      return;
    }

    esp_ota_handle_t otaHandle = 0;
    const esp_err_t err = esp_ota_begin(updatePartition, firmwareExpectedSize, &otaHandle);
    if (err != ESP_OK) {
      firmwareUploadError = std::string("Could not open OTA partition: ") + esp_err_to_name(err);
      abortFirmwareUpload(firmwareUploadError.c_str());
      return;
    }

    firmwareOtaHandle = static_cast<uint32_t>(otaHandle);
    firmwareOtaPartition = updatePartition;
    firmwareOtaActive = true;
    firmwareUploadState = FirmwareUploadState::RECEIVING;
    Serial.printf("[%lu] [WEB] [UPDATE] Receiving %s (%u bytes) into %s\n", millis(), firmwareUploadName.c_str(),
                  static_cast<unsigned>(firmwareExpectedSize), updatePartition->label);
#endif
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (firmwareUploadState != FirmwareUploadState::RECEIVING || !firmwareOtaActive) {
      return;
    }
    if (upload.currentSize == 0) {
      return;
    }
    if (firmwareReceivedSize + upload.currentSize > firmwareExpectedSize) {
      abortFirmwareUpload("Upload exceeded the declared firmware size");
      return;
    }
    if (firmwareReceivedSize == 0) {
      if (upload.currentSize < 14 || upload.buf[0] != ESP_IMAGE_MAGIC) {
        abortFirmwareUpload("File is not an ESP32 application image");
        return;
      }
      const uint16_t chipId = static_cast<uint16_t>(upload.buf[12]) | (static_cast<uint16_t>(upload.buf[13]) << 8);
      if (chipId != ESP32_C3_CHIP_ID) {
        abortFirmwareUpload("Firmware target is not ESP32-C3");
        return;
      }
    }

#ifndef SIMULATOR
    const esp_err_t err =
        esp_ota_write(static_cast<esp_ota_handle_t>(firmwareOtaHandle), upload.buf, upload.currentSize);
    if (err != ESP_OK) {
      firmwareUploadError = std::string("Flash write failed: ") + esp_err_to_name(err);
      abortFirmwareUpload(firmwareUploadError.c_str());
      return;
    }
#endif
    firmwareReceivedSize += upload.currentSize;
    esp_task_wdt_reset();
    return;
  }

  if (upload.status == UPLOAD_FILE_END) {
    if (firmwareUploadState != FirmwareUploadState::RECEIVING || !firmwareOtaActive) {
      return;
    }
    if (firmwareReceivedSize != firmwareExpectedSize) {
      abortFirmwareUpload("Firmware upload ended before all bytes were received");
      return;
    }

#ifndef SIMULATOR
    const auto* updatePartition = static_cast<const esp_partition_t*>(firmwareOtaPartition);
    esp_err_t err = esp_ota_end(static_cast<esp_ota_handle_t>(firmwareOtaHandle));
    firmwareOtaActive = false;
    firmwareOtaHandle = 0;
    if (err != ESP_OK) {
      firmwareOtaPartition = nullptr;
      firmwareUploadError = std::string("Firmware validation failed: ") + esp_err_to_name(err);
      abortFirmwareUpload(firmwareUploadError.c_str());
      return;
    }

    esp_app_desc_t description = {};
    if (esp_ota_get_partition_description(updatePartition, &description) == ESP_OK) {
      firmwareUploadVersion = description.version;
    }

    err = esp_ota_set_boot_partition(updatePartition);
    if (err != ESP_OK) {
      firmwareOtaPartition = nullptr;
      firmwareUploadError = std::string("Could not select the new boot partition: ") + esp_err_to_name(err);
      abortFirmwareUpload(firmwareUploadError.c_str());
      return;
    }
#endif

    firmwareUploadState = FirmwareUploadState::READY_TO_REBOOT;
    Serial.printf("[%lu] [WEB] [UPDATE] Firmware validated; reboot target is ready\n", millis());
    return;
  }

  if (upload.status == UPLOAD_FILE_ABORTED) {
    abortFirmwareUpload("Firmware upload was cancelled");
  }
}

void LocalServer::handleFirmwareUploadPost() {
  if (firmwareUploadState != FirmwareUploadState::READY_TO_REBOOT) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = firmwareUploadError.empty() ? "Firmware upload did not complete" : firmwareUploadError;
    String json;
    serializeJson(doc, json);
    server->send(400, "application/json", json);
    return;
  }

  firmwareRestartAt = millis() + 2500;
  JsonDocument doc;
  doc["ok"] = true;
  doc["received"] = firmwareReceivedSize;
  doc["version"] = firmwareUploadVersion;
  doc["rebootInMs"] = 2500;
  String json;
  serializeJson(doc, json);
  server->sendHeader("Cache-Control", "no-store");
  server->sendHeader("Connection", "close");
  server->send(200, "application/json", json);
}

void LocalServer::scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const {
  FsFile root = SdMan.open(path);
  if (!root) {
    Serial.printf("[%lu] [WEB] Failed to open directory: %s\n", millis(), path);
    return;
  }

  if (!root.isDirectory()) {
    Serial.printf("[%lu] [WEB] Not a directory: %s\n", millis(), path);
    root.close();
    return;
  }

  Serial.printf("[%lu] [WEB] Scanning files in: %s\n", millis(), path);

  FsFile file = root.openNextFile();
  char name[500];
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    bool shouldHide = fileName.startsWith(".");

    if (!shouldHide) {
      for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
        if (fileName.equals(HIDDEN_ITEMS[i])) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      callback(info);
    }

    file.close();
    yield();
    esp_task_wdt_reset();
    file = root.openNextFile();
  }
  root.close();
}

bool LocalServer::isEpubFile(const String& filename) const {
  std::string lower = filename.c_str();
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".epub") == 0;
}

void LocalServer::handleFileList() const { server->send(200, "text/html", FilesPageHtml); }

void LocalServer::handleLibraryPage() const {
  server->send_P(200, PSTR("text/html; charset=utf-8"), LibraryPageHtml, sizeof(LibraryPageHtml) - 1);
}

void LocalServer::handleEpubPage() const {
  server->send_P(200, PSTR("text/html; charset=utf-8"), EpubPageHtml, sizeof(EpubPageHtml) - 1);
}

void LocalServer::handleExportPage() const {
  server->send_P(200, PSTR("text/html; charset=utf-8"), ExportPageHtml, sizeof(ExportPageHtml) - 1);
}

void LocalServer::handleFontManagerPage() const { server->send(200, "text/html", FontManagerPageHtml); }

void LocalServer::handleTagsPage() const { server->send(200, "text/html", TagsPageHtml); }

void LocalServer::handleInxFontPackJs() const {
  server->send_P(200, PSTR("text/javascript; charset=utf-8"), INX_FONT_PACK_JS, sizeof(INX_FONT_PACK_JS) - 1);
}

void LocalServer::handleJsZipMinJs() const {
  server->send_P(200, PSTR("text/javascript; charset=utf-8"), JSZIP_MIN_JS, sizeof(JSZIP_MIN_JS) - 1);
}

void LocalServer::handleEpubPageJs() const {
  server->send_P(200, PSTR("text/javascript; charset=utf-8"), EPUB_PAGE_JS, sizeof(EPUB_PAGE_JS) - 1);
}

void LocalServer::handleFilesPageJs() const {
  server->send_P(200, PSTR("text/javascript; charset=utf-8"), FILES_PAGE_JS, sizeof(FILES_PAGE_JS) - 1);
}

void LocalServer::handleLibraryPageJs() const {
  server->send_P(200, PSTR("text/javascript; charset=utf-8"), LIBRARY_PAGE_JS, sizeof(LIBRARY_PAGE_JS) - 1);
}

void LocalServer::handleFileListData() const {
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = server->arg("path");

    if (!currentPath.startsWith("/")) {
      currentPath = "/" + currentPath;
    }

    if (currentPath.length() > 1 && currentPath.endsWith("/")) {
      currentPath = currentPath.substring(0, currentPath.length() - 1);
    }
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  scanFiles(currentPath.c_str(), [this, &output, &doc, seenFirst](const FileInfo& info) mutable {
    doc.clear();
    doc["name"] = info.name;
    doc["size"] = info.size;
    doc["isDirectory"] = info.isDirectory;
    doc["isEpub"] = info.isEpub;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      Serial.printf("[%lu] [WEB] Skipping file entry with oversized JSON for name: %s\n", millis(), info.name.c_str());
      return;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  });
  server->sendContent("]");

  server->sendContent("");
  Serial.printf("[%lu] [WEB] Served file listing page for path: %s\n", millis(), currentPath.c_str());
}

void LocalServer::handleLibraryData() const {
#ifdef INX_SIMULATOR_WEB_ONLY
  server->send(200, "application/json",
               "{\"indexed\":true,\"indexing\":false,\"current\":0,\"total\":0,\"books\":["
               "{\"path\":\"/Books/1984.epub\",\"title\":\"1984\",\"author\":\"George Orwell\",\"folder\":\"Books\","
               "\"tag\":\"Classic\",\"type\":\"EPUB\",\"size\":1048576,\"coverUrl\":\"\",\"progress\":64,"
               "\"readingTimeMs\":7200000,\"pagesRead\":126,\"sessions\":8},"
               "{\"path\":\"/Books/Sample.xtc\",\"title\":\"Sample Panels\",\"author\":\"Inx\",\"folder\":\"Books\","
               "\"tag\":\"Manga\",\"type\":\"XTC\",\"size\":524288,\"coverUrl\":\"\",\"progress\":12,"
               "\"readingTimeMs\":900000,\"pagesRead\":18,\"sessions\":2}]}");
#else
  std::vector<IndexedBookInfo> books;
  const bool hasIndex = loadIndexedBooksWithTags(books);
  std::sort(books.begin(), books.end(), [](const IndexedBookInfo& a, const IndexedBookInfo& b) {
    return lowerAscii(a.title) < lowerAscii(b.title);
  });

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("{\"indexed\":");
  server->sendContent(hasIndex ? "true" : "false");
  server->sendContent(",\"indexing\":");
  server->sendContent(webLibraryIndexing ? "true" : "false");
  server->sendContent(",\"current\":");
  server->sendContent(String(webLibraryIndexCurrent));
  server->sendContent(",\"total\":");
  server->sendContent(String(webLibraryIndexTotal));
  server->sendContent(",\"books\":[");

  bool first = true;
  for (const IndexedBookInfo& book : books) {
    const std::string cachePath = cachePathForIndexedBook(book.path);
    String title = book.title;
    String author;
    BookMetadataCache metadata(cachePath);
    if (metadata.load()) {
      if (!metadata.coreMetadata.title.empty()) {
        title = metadata.coreMetadata.title.c_str();
      }
      author = metadata.coreMetadata.author.c_str();
    }

    BookReadingStats stats;
    const bool hasStats = loadBookStats(cachePath.c_str(), stats);
    if (hasStats) {
      if (title.isEmpty() && !stats.title.empty()) {
        title = stats.title.c_str();
      }
      if (author.isEmpty() && !stats.author.empty()) {
        author = stats.author.c_str();
      }
    }

    if (!first) {
      server->sendContent(",");
    }
    first = false;

    String row = "{\"path\":\"";
    row += jsonEscape(book.path);
    row += "\",\"title\":\"";
    row += jsonEscape(title);
    row += "\",\"author\":\"";
    row += jsonEscape(author);
    row += "\",\"folder\":\"";
    row += jsonEscape(book.folder);
    row += "\",\"tag\":\"";
    row += jsonEscape(book.tag);
    row += "\",\"type\":\"";
    row += bookFileType(book.path);
    row += "\",\"size\":";
    row += String(static_cast<unsigned long>(fileSizeForPath(book.path)));
    row += ",\"coverUrl\":\"";
    row += jsonEscape(coverUrlForCachePath(cachePath));
    row += "\",\"progress\":";
    row += String(hasStats ? std::max(0.0f, std::min(100.0f, stats.progressPercent)) : 0.0f, 1);
    row += ",\"readingTimeMs\":";
    row += String(hasStats ? stats.totalReadingTimeMs : 0);
    row += ",\"pagesRead\":";
    row += String(hasStats ? stats.totalPagesRead : 0);
    row += ",\"sessions\":";
    row += String(hasStats ? stats.sessionCount : 0);
    row += "}";
    server->sendContent(row);
    yield();
  }
  server->sendContent("]}");
  server->sendContent("");
#endif
}

void LocalServer::handleExportNotesData() const {
#ifdef INX_SIMULATOR_WEB_ONLY
  server->send(501, "application/json", "{\"ok\":false,\"error\":\"unavailable_in_simulator\"}");
#else
  const bool forceRefresh = server->hasArg("refresh") && server->arg("refresh") == "1";
  if (forceRefresh) {
    EpubNotesIndex::invalidate();
  }

  if (!exportNotesIndexIsCurrent()) {
    EpubNotesIndex::invalidate();
  }

  if (!SdMan.exists(EpubNotesIndex::kPath) && !buildExportNotesIndex()) {
    server->send(500, "application/json", "{\"ok\":false,\"error\":\"index_failed\"}");
    return;
  }

  FsFile index;
  if (!SdMan.openFileForRead("EXP", EpubNotesIndex::kPath, index)) {
    server->send(500, "application/json", "{\"ok\":false,\"error\":\"index_unreadable\"}");
    return;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  char buf[513];
  while (index.available()) {
    const int n = index.read(buf, sizeof(buf) - 1);
    if (n <= 0) {
      break;
    }
    buf[n] = '\0';
    server->sendContent(buf);
    yield();
  }
  index.close();
  server->sendContent("");
#endif
}

void LocalServer::handleBookTagsGet() const {
#ifdef INX_SIMULATOR_WEB_ONLY
  server->send(501, "application/json", "{\"ok\":false,\"error\":\"unavailable_in_simulator\"}");
#else
  std::vector<IndexedBookInfo> books;
  const bool hasIndex = loadIndexedBooksWithTags(books);
  std::vector<std::string> tags;
  BookTags::loadTagList(tags);

  std::sort(books.begin(), books.end(), [](const IndexedBookInfo& a, const IndexedBookInfo& b) {
    return lowerAscii(a.title) < lowerAscii(b.title);
  });

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("{\"indexed\":");
  server->sendContent(hasIndex ? "true" : "false");
  server->sendContent(",\"tags\":[");
  for (size_t i = 0; i < tags.size(); ++i) {
    if (i > 0) {
      server->sendContent(",");
    }
    server->sendContent("\"" + jsonEscape(tags[i].c_str()) + "\"");
  }
  server->sendContent("],\"books\":[");
  bool first = true;
  for (const auto& book : books) {
    if (!first) {
      server->sendContent(",");
    }
    first = false;
    String row = "{\"path\":\"" + jsonEscape(book.path) + "\",\"title\":\"" + jsonEscape(book.title) +
                 "\",\"folder\":\"" + jsonEscape(book.folder) + "\",\"tag\":\"" + jsonEscape(book.tag) + "\"}";
    server->sendContent(row);
    yield();
  }
  server->sendContent("]}");
  server->sendContent("");
#endif
}

void LocalServer::handleLibraryIndexRefresh() const {
#ifdef INX_SIMULATOR_WEB_ONLY
  server->send(501, "application/json", "{\"ok\":false,\"error\":\"unavailable_in_simulator\"}");
#else
  if (webLibraryIndexing) {
    server->send(200, "application/json", "{\"ok\":true,\"indexing\":true}");
    return;
  }

  webLibraryIndexing = true;
  webLibraryIndexCurrent = 0;
  webLibraryIndexTotal = 0;
  webLibraryIndexPath[0] = '\0';

  BaseType_t created = xTaskCreate(webLibraryIndexTask, "WebLibIndex", 4096, nullptr, 1, nullptr);
  if (created != pdPASS) {
    webLibraryIndexing = false;
    server->send(500, "application/json", "{\"ok\":false,\"error\":\"task\"}");
    return;
  }

  server->send(200, "application/json", "{\"ok\":true,\"indexing\":true}");
#endif
}

void LocalServer::handleLibraryIndexStatus() const {
#ifdef INX_SIMULATOR_WEB_ONLY
  server->send(200, "application/json", "{\"indexing\":false,\"current\":0,\"total\":0,\"path\":\"\"}");
#else
  String body = "{\"indexing\":";
  body += webLibraryIndexing ? "true" : "false";
  body += ",\"current\":";
  body += String(webLibraryIndexCurrent);
  body += ",\"total\":";
  body += String(webLibraryIndexTotal);
  body += ",\"path\":\"";
  body += jsonEscape(String(webLibraryIndexPath));
  body += "\"}";
  server->send(200, "application/json", body);
#endif
}

void LocalServer::handleBookTagsPost() const {
#ifdef INX_SIMULATOR_WEB_ONLY
  server->send(501, "application/json", "{\"ok\":false,\"error\":\"unavailable_in_simulator\"}");
#else
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, server->arg("plain"));
  if (err) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }

  const char* action = doc["action"] | "";
  if (strcmp(action, "addTag") == 0) {
    const char* tag = doc["tag"] | "";
    if (!BookTags::addTag(tag)) {
      server->send(500, "text/plain", "Failed to save tag");
      return;
    }
    server->send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (strcmp(action, "renameTag") == 0) {
    const char* oldTag = doc["oldTag"] | "";
    const char* newTag = doc["newTag"] | "";
    if (!BookTags::renameTag(oldTag, newTag)) {
      server->send(500, "text/plain", "Failed to rename tag");
      return;
    }
    server->send(200, "application/json", "{\"ok\":true}");
    return;
  }

  if (strcmp(action, "deleteTag") == 0) {
    const char* tag = doc["tag"] | "";
    if (!BookTags::deleteTag(tag)) {
      server->send(500, "text/plain", "Failed to delete tag");
      return;
    }
    server->send(200, "application/json", "{\"ok\":true}");
    return;
  }

  const char* path = doc["path"] | "";
  const char* tag = doc["tag"] | "";
  if (!path[0]) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  if (!BookTags::set(path, tag)) {
    server->send(500, "text/plain", "Failed to save tag");
    return;
  }

  server->send(200, "application/json", "{\"ok\":true}");
#endif
}

void LocalServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (itemName.startsWith(".")) {
    server->send(403, "text/plain", "Cannot access system files");
    return;
  }
  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (itemName.equals(HIDDEN_ITEMS[i])) {
      server->send(403, "text/plain", "Cannot access protected items");
      return;
    }
  }

  if (!SdMan.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = SdMan.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  } else if (itemPath.endsWith(".jpg") || itemPath.endsWith(".jpeg") || itemPath.endsWith(".JPG") ||
             itemPath.endsWith(".JPEG")) {
    contentType = "image/jpeg";
  } else if (itemPath.endsWith(".png") || itemPath.endsWith(".PNG")) {
    contentType = "image/png";
  } else if (itemPath.endsWith(".bmp") || itemPath.endsWith(".BMP")) {
    contentType = "image/bmp";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  const bool inlineView = server->hasArg("inline") && server->arg("inline") == "1";
  server->sendHeader("Content-Disposition",
                     String(inlineView ? "inline" : "attachment") + "; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  WiFiClient client = server->client();
  client.write(file);
  file.close();
}

static FsFile uploadFile;
static String uploadFileName;
static String uploadPath = "/";
static size_t uploadSize = 0;
static bool uploadSuccess = false;
static String uploadError = "";

constexpr size_t UPLOAD_BUFFER_SIZE = 4096;
static uint8_t* uploadBuffer = nullptr;
static size_t uploadBufferPos = 0;

static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static void freeUploadBuffer() {
  std::free(uploadBuffer);
  uploadBuffer = nullptr;
  uploadBufferPos = 0;
}

static bool flushUploadBuffer() {
  if (uploadBufferPos > 0 && uploadFile && uploadBuffer) {
    esp_task_wdt_reset();
    const unsigned long writeStart = millis();
    const size_t written = uploadFile.write(uploadBuffer, uploadBufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    esp_task_wdt_reset();

    if (written != uploadBufferPos) {
      Serial.printf("[%lu] [WEB] [UPLOAD] Buffer flush failed: expected %d, wrote %d\n", millis(), uploadBufferPos,
                    written);
      uploadBufferPos = 0;
      return false;
    }
    uploadBufferPos = 0;
    yield();
  }
  return true;
}

void LocalServer::handleUpload() const {
  static size_t lastLoggedSize = 0;

  esp_task_wdt_reset();

  if (!running || !server) {
    Serial.printf("[%lu] [WEB] [UPLOAD] ERROR: handleUpload called but server not running!\n", millis());
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    esp_task_wdt_reset();

    uploadFileName = upload.filename;
    uploadSize = 0;
    uploadSuccess = false;
    uploadError = "";
    uploadStartTime = millis();
    lastLoggedSize = 0;
    uploadBufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;
    freeUploadBuffer();

    if (server->hasArg("path")) {
      uploadPath = server->arg("path");

      if (!uploadPath.startsWith("/")) {
        uploadPath = "/" + uploadPath;
      }

      if (uploadPath.length() > 1 && uploadPath.endsWith("/")) {
        uploadPath = uploadPath.substring(0, uploadPath.length() - 1);
      }
    } else {
      uploadPath = "/";
    }

    Serial.printf("[%lu] [WEB] [UPLOAD] START: %s to path: %s\n", millis(), uploadFileName.c_str(), uploadPath.c_str());
    Serial.printf("[%lu] [WEB] [UPLOAD] Free heap: %d bytes\n", millis(), ESP.getFreeHeap());

    String filePath = uploadPath;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += uploadFileName;

    esp_task_wdt_reset();
    if (SdMan.exists(filePath.c_str())) {
      Serial.printf("[%lu] [WEB] [UPLOAD] Overwriting existing file: %s\n", millis(), filePath.c_str());
      esp_task_wdt_reset();
      SdMan.remove(filePath.c_str());
    }

    esp_task_wdt_reset();
    if (!SdMan.openFileForWrite("WEB", filePath, uploadFile)) {
      uploadError = "Failed to create file on SD card";
      Serial.printf("[%lu] [WEB] [UPLOAD] FAILED to create file: %s\n", millis(), filePath.c_str());
      return;
    }
    esp_task_wdt_reset();

    uploadBuffer = static_cast<uint8_t*>(std::malloc(UPLOAD_BUFFER_SIZE));
    if (!uploadBuffer) {
      uploadError = "Failed to allocate upload buffer";
      uploadFile.close();
      SdMan.remove(filePath.c_str());
      Serial.printf("[%lu] [WEB] [UPLOAD] FAILED to allocate %d byte buffer\n", millis(), UPLOAD_BUFFER_SIZE);
      return;
    }

    Serial.printf("[%lu] [WEB] [UPLOAD] File created successfully: %s\n", millis(), filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadError.isEmpty()) {
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UPLOAD_BUFFER_SIZE - uploadBufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(uploadBuffer + uploadBufferPos, data, toCopy);
        uploadBufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        if (uploadBufferPos >= UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer()) {
            uploadError = "Failed to write to SD card - disk may be full";
            uploadFile.close();
            return;
          }
        }
      }

      uploadSize += upload.currentSize;

      if (uploadSize - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (uploadSize / 1024.0) / (elapsed / 1000.0) : 0;
        Serial.printf("[%lu] [WEB] [UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes\n", millis(), uploadSize,
                      uploadSize / 1024.0, kbps, writeCount);
        lastLoggedSize = uploadSize;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      if (!flushUploadBuffer()) {
        uploadError = "Failed to write final data to SD card";
      }
      uploadFile.close();
      freeUploadBuffer();

      if (uploadError.isEmpty()) {
        uploadSuccess = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (uploadSize / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        Serial.printf("[%lu] [WEB] [UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)\n", millis(),
                      uploadFileName.c_str(), uploadSize, elapsed, avgKbps);
        Serial.printf("[%lu] [WEB] [UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)\n", millis(),
                      writeCount, totalWriteTime, writePercent);

        String filePath = uploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += uploadFileName;
        clearEpubCacheIfNeeded(filePath);
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    freeUploadBuffer();
    if (uploadFile) {
      uploadFile.close();

      String filePath = uploadPath;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += uploadFileName;
      SdMan.remove(filePath.c_str());
    }
    uploadError = "Upload aborted";
    Serial.printf("[%lu] [WEB] Upload aborted\n", millis());
  }
}

void LocalServer::handleUploadPost() const {
  if (uploadSuccess) {
    server->send(200, "text/plain", "File uploaded successfully: " + uploadFileName);
  } else {
    const String error = uploadError.isEmpty() ? "Unknown error during upload" : uploadError;
    server->send(400, "text/plain", error);
  }
}

void LocalServer::handleCreateFolder() const {
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = server->arg("name");

  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }

  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = server->arg("path");
    if (!parentPath.startsWith("/")) {
      parentPath = "/" + parentPath;
    }
    if (parentPath.length() > 1 && parentPath.endsWith("/")) {
      parentPath = parentPath.substring(0, parentPath.length() - 1);
    }
  }

  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  Serial.printf("[%lu] [WEB] Creating folder: %s\n", millis(), folderPath.c_str());

  if (SdMan.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  if (SdMan.mkdir(folderPath.c_str())) {
    Serial.printf("[%lu] [WEB] Folder created successfully: %s\n", millis(), folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    Serial.printf("[%lu] [WEB] Failed to create folder: %s\n", millis(), folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void LocalServer::handleDelete() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = server->arg("path");
  const String itemType = server->hasArg("type") ? server->arg("type") : "file";

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Cannot delete root directory");
    return;
  }

  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  if (itemName.startsWith(".")) {
    Serial.printf("[%lu] [WEB] Delete rejected - hidden/system item: %s\n", millis(), itemPath.c_str());
    server->send(403, "text/plain", "Cannot delete system files");
    return;
  }

  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (itemName.equals(HIDDEN_ITEMS[i])) {
      Serial.printf("[%lu] [WEB] Delete rejected - protected item: %s\n", millis(), itemPath.c_str());
      server->send(403, "text/plain", "Cannot delete protected items");
      return;
    }
  }

  if (!SdMan.exists(itemPath.c_str())) {
    Serial.printf("[%lu] [WEB] Delete failed - item not found: %s\n", millis(), itemPath.c_str());
    server->send(404, "text/plain", "Item not found");
    return;
  }

  Serial.printf("[%lu] [WEB] Attempting to delete %s: %s\n", millis(), itemType.c_str(), itemPath.c_str());

  bool success = false;

  if (itemType == "folder") {
    FsFile dir = SdMan.open(itemPath.c_str());
    if (dir && dir.isDirectory()) {
      FsFile entry = dir.openNextFile();
      if (entry) {
        entry.close();
        dir.close();
        Serial.printf("[%lu] [WEB] Delete failed - folder not empty: %s\n", millis(), itemPath.c_str());
        server->send(400, "text/plain", "Folder is not empty. Delete contents first.");
        return;
      }
      dir.close();
    }
    success = SdMan.rmdir(itemPath.c_str());
  } else {
    success = SdMan.remove(itemPath.c_str());
  }

  if (success) {
    Serial.printf("[%lu] [WEB] Successfully deleted: %s\n", millis(), itemPath.c_str());
    server->send(200, "text/plain", "Deleted successfully");
  } else {
    Serial.printf("[%lu] [WEB] Failed to delete: %s\n", millis(), itemPath.c_str());
    server->send(500, "text/plain", "Failed to delete item");
  }
}

void LocalServer::collectEpubRenames(const std::string& oldDirPath, const std::string& newDirPath,
                                     std::vector<std::pair<std::string, std::string>>& out) const {
  scanFiles(oldDirPath.c_str(), [&](const FileInfo info) {
    const std::string childOld = oldDirPath + "/" + info.name.c_str();
    const std::string childNew = newDirPath + "/" + info.name.c_str();
    if (info.isDirectory) {
      collectEpubRenames(childOld, childNew, out);
    } else if (info.isEpub) {
      out.emplace_back(childOld, childNew);
    }
  });
}

void LocalServer::migrateEpubBookState(const std::string& oldPath, const std::string& newPath) const {
#ifndef INX_SIMULATOR_WEB_ONLY
  // Bookmarks, annotations, reading progress, and book settings all live under a cache dir keyed by
  // hash(filepath) (see Epub::Epub) - move it alongside the file so a rename doesn't orphan that state.
  const std::string oldCachePath = "/.metadata/epub/" + std::to_string(std::hash<std::string>{}(oldPath));
  const std::string newCachePath = "/.metadata/epub/" + std::to_string(std::hash<std::string>{}(newPath));

  if (SdMan.exists(oldCachePath.c_str()) && !SdMan.exists(newCachePath.c_str())) {
    SdMan.rename(oldCachePath.c_str(), newCachePath.c_str());
  }

  BOOK_STATE.renamePath(oldPath, newPath);
  RECENT_BOOKS.renamePath(oldPath, newPath, newCachePath);
#else
  (void)oldPath;
  (void)newPath;
#endif
}

void LocalServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or name");
    return;
  }

  String itemPath = server->arg("path");
  String newName = server->arg("name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Cannot rename root directory");
    return;
  }
  if (!itemPath.startsWith("/")) {
    itemPath = "/" + itemPath;
  }
  if (itemPath.length() > 1 && itemPath.endsWith("/")) {
    itemPath = itemPath.substring(0, itemPath.length() - 1);
  }

  if (newName.isEmpty() || newName.indexOf('/') != -1 || newName.indexOf('\\') != -1 || newName == "." ||
      newName == "..") {
    server->send(400, "text/plain", "Invalid name");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  if (itemName.startsWith(".")) {
    Serial.printf("[%lu] [WEB] Rename rejected - hidden/system item: %s\n", millis(), itemPath.c_str());
    server->send(403, "text/plain", "Cannot rename system files");
    return;
  }

  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (itemName.equals(HIDDEN_ITEMS[i])) {
      Serial.printf("[%lu] [WEB] Rename rejected - protected item: %s\n", millis(), itemPath.c_str());
      server->send(403, "text/plain", "Cannot rename protected items");
      return;
    }
  }

  if (!SdMan.exists(itemPath.c_str())) {
    Serial.printf("[%lu] [WEB] Rename failed - item not found: %s\n", millis(), itemPath.c_str());
    server->send(404, "text/plain", "Item not found");
    return;
  }

  const int lastSlash = itemPath.lastIndexOf('/');
  const String parentPath = lastSlash > 0 ? itemPath.substring(0, lastSlash) : String("");
  const String newPath = parentPath + "/" + newName;

  if (newPath == itemPath) {
    server->send(200, "text/plain", "Renamed successfully");
    return;
  }

  // FAT/exFAT lookups are case-insensitive, so a pure case change (Foo.epub -> FOO.epub) would otherwise
  // collide with itself here; only block on a genuine name clash.
  if (strcasecmp(newName.c_str(), itemName.c_str()) != 0 && SdMan.exists(newPath.c_str())) {
    server->send(409, "text/plain", "An item with that name already exists");
    return;
  }

  FsFile item = SdMan.open(itemPath.c_str());
  const bool isDir = item && item.isDirectory();
  if (item) {
    item.close();
  }

  std::vector<std::pair<std::string, std::string>> epubRenames;
  if (isDir) {
    collectEpubRenames(itemPath.c_str(), newPath.c_str(), epubRenames);
  } else if (isEpubFile(itemName)) {
    epubRenames.emplace_back(itemPath.c_str(), newPath.c_str());
  }

  Serial.printf("[%lu] [WEB] Renaming %s -> %s\n", millis(), itemPath.c_str(), newPath.c_str());

  if (!SdMan.rename(itemPath.c_str(), newPath.c_str())) {
    Serial.printf("[%lu] [WEB] Failed to rename: %s\n", millis(), itemPath.c_str());
    server->send(500, "text/plain", "Failed to rename item");
    return;
  }

  for (const auto& renamePair : epubRenames) {
    migrateEpubBookState(renamePair.first, renamePair.second);
  }

  Serial.printf("[%lu] [WEB] Successfully renamed: %s -> %s\n", millis(), itemPath.c_str(), newPath.c_str());
  server->send(200, "text/plain", "Renamed successfully");
}

void LocalServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

void LocalServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      Serial.printf("[%lu] [WS] Client %u disconnected\n", millis(), num);

      if (wsUploadInProgress && wsUploadFile) {
        wsUploadFile.close();

        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        SdMan.remove(filePath.c_str());
        Serial.printf("[%lu] [WS] Deleted incomplete upload: %s\n", millis(), filePath.c_str());
      }
      wsUploadInProgress = false;
      break;

    case WStype_CONNECTED: {
      Serial.printf("[%lu] [WS] Client %u connected\n", millis(), num);
      break;
    }

    case WStype_TEXT: {
      String msg = String((char*)payload);
      Serial.printf("[%lu] [WS] Text from client %u: %s\n", millis(), num, msg.c_str());

      if (msg.startsWith("START:")) {
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = msg.substring(6, firstColon);
          wsUploadSize = msg.substring(firstColon + 1, secondColon).toInt();
          wsUploadPath = msg.substring(secondColon + 1);
          wsUploadReceived = 0;
          wsUploadStartTime = millis();

          if (!wsUploadPath.startsWith("/")) wsUploadPath = "/" + wsUploadPath;
          if (wsUploadPath.length() > 1 && wsUploadPath.endsWith("/")) {
            wsUploadPath = wsUploadPath.substring(0, wsUploadPath.length() - 1);
          }

          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          Serial.printf("[%lu] [WS] Starting upload: %s (%d bytes) to %s\n", millis(), wsUploadFileName.c_str(),
                        wsUploadSize, filePath.c_str());

          esp_task_wdt_reset();
          if (SdMan.exists(filePath.c_str())) {
            SdMan.remove(filePath.c_str());
          }

          esp_task_wdt_reset();
          if (!SdMan.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            return;
          }
          esp_task_wdt_reset();

          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      esp_task_wdt_reset();
      size_t written = wsUploadFile.write(payload, length);
      esp_task_wdt_reset();

      if (written != length) {
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      static size_t lastProgressSent = 0;
      if (wsUploadReceived - lastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        lastProgressSent = wsUploadReceived;
      }

      if (wsUploadReceived >= wsUploadSize) {
        wsUploadFile.close();
        wsUploadInProgress = false;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        Serial.printf("[%lu] [WS] Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)\n", millis(),
                      wsUploadFileName.c_str(), wsUploadSize, elapsed, kbps);

        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearEpubCacheIfNeeded(filePath);

        wsServer->sendTXT(num, "DONE");
        lastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

void LocalServer::handleSettingsPage() const {
  server->send(200, "text/html", SettingsPageHtml);
  Serial.printf("[%lu] [WEB] Served settings page\n", millis());
}

void LocalServer::handleSettingsGet() const {
  JsonDocument doc;
  const bool clockAvailable = clockSettingsAvailable();
  const uint8_t sleepScreen = (!clockAvailable && SETTINGS.sleepScreen == SystemSetting::DATETIME)
                                  ? SystemSetting::LIGHT
                                  : SETTINGS.sleepScreen;

  doc["clockAvailable"] = clockAvailable;
  doc["sleepScreen"] = sleepScreen;
  doc["sleepScreenCoverMode"] = SETTINGS.sleepScreenCoverMode;
  doc["sleepScreenCoverFilter"] = SETTINGS.sleepScreenCoverFilter;
  doc["sleepImageQuality"] = SETTINGS.sleepImageQuality;
  doc["sleepScreenCoverGrayscale"] = SETTINGS.sleepImageQuality;
  doc["sleepImageTwoBit"] = SETTINGS.sleepImageQuality != SystemSetting::SLEEP_IMAGE_LOW;
  doc["sleepCustomBmp"] = SETTINGS.sleepCustomBmp;
  doc["sleepImageRotationEnabled"] = SETTINGS.sleepImageRotationEnabled;
  doc["sleepImageRotationMinutes"] = SETTINGS.getSleepImageRotationMinutes();
  doc["sleepImagePowerDoublePress"] = SETTINGS.sleepImagePowerDoublePress;
  doc["sleepImagePowerGestureWindow"] = SETTINGS.sleepImagePowerGestureWindow;
  doc["sleepImagePowerFirstPressMin"] = SETTINGS.sleepImagePowerFirstPressMin;
  doc["sleepImagePowerSecondPressMax"] = SETTINGS.sleepImagePowerSecondPressMax;
  doc["persistentSleepLogs"] = SETTINGS.persistentSleepLogs;
  doc["lastSleepTimerArmSeconds"] = APP_STATE.lastSleepTimerArmSeconds;
  doc["sleepTimerArmCount"] = APP_STATE.sleepTimerArmCount;
  doc["sleepTimerWakeCount"] = APP_STATE.sleepTimerWakeCount;
  doc["lastWakeReason"] = APP_STATE.lastWakeReason;
  doc["lastSleepImagePath"] = APP_STATE.lastSleepImagePath;
#ifndef INX_SIMULATOR_WEB_ONLY
  const HalGPIO::DeepSleepDiagnostics deepSleepDiagnostics = gpio.getDeepSleepDiagnostics();
  doc["deepSleepRequestedTimerSeconds"] = deepSleepDiagnostics.requestedTimerSeconds;
  doc["deepSleepGpioSetupResult"] = deepSleepDiagnostics.gpioSetupResult;
  doc["deepSleepTimerSetupResult"] = deepSleepDiagnostics.timerSetupResult;
  doc["deepSleepWakeStubCount"] = deepSleepDiagnostics.wakeStubCount;
  doc["deepSleepTimerWakeStubCount"] = deepSleepDiagnostics.timerWakeStubCount;
  doc["deepSleepLastWakeStubCause"] = deepSleepDiagnostics.lastWakeStubCause;
#endif
  if (clockAvailable) {
    doc["sleepClockStyle"] = SETTINGS.sleepClockStyle;
    doc["sleepClockTimeFormat"] = SETTINGS.sleepClockTimeFormat;
    doc["timeZoneQuarterOffset"] = SETTINGS.timeZoneQuarterOffset;
  }
  doc["hideBatteryPercentage"] = SETTINGS.hideBatteryPercentage;
  doc["uiTheme"] = SETTINGS.uiTheme;
  doc["showBottomBarClock"] = SETTINGS.showBottomBarClock;
  doc["recentLibraryMode"] = SETTINGS.recentLibraryMode;
  doc["libraryMode"] = SETTINGS.libraryMode;
  doc["recentVisibleCount"] = SETTINGS.recentVisibleCount;
  doc["librarySortEnabled"] = SETTINGS.librarySortEnabled;
  doc["libraryShelfEnabled"] = SETTINGS.libraryShelfEnabled;
  doc["librarySortMode"] = SETTINGS.librarySortMode;

  doc["fontFamily"] = SETTINGS.fontFamily;
  doc["fontSize"] = SETTINGS.fontSize;

  doc["lineHeight"] = SETTINGS.lineHeight;
  doc["textSpace"] = SETTINGS.textSpace;
  doc["screenMargin"] = SETTINGS.screenMargin;
  doc["paragraphAlignment"] = SETTINGS.paragraphAlignment;
  doc["paragraphCssIndentEnabled"] = SETTINGS.paragraphCssIndentEnabled;
  doc["extraParagraphSpacing"] = SETTINGS.extraParagraphSpacing;
  doc["orientation"] = SETTINGS.orientation;
  doc["hyphenationEnabled"] = SETTINGS.hyphenationEnabled;
  doc["bionicReadingEnabled"] = SETTINGS.bionicReadingEnabled;

  doc["readerDirectionMapping"] = SETTINGS.readerDirectionMapping;
  doc["readerMenuButton"] = SETTINGS.readerMenuButton;
  doc["longPressChapterSkip"] = SETTINGS.longPressChapterSkip;
  doc["readerShortPwrBtn"] = SETTINGS.readerShortPwrBtn;
  doc["shakePageTurn"] = SETTINGS.shakePageTurn;
  doc["shakePageTurnSensitivity"] = SETTINGS.shakePageTurnSensitivity;

  doc["textAntiAliasing"] = SETTINGS.textAntiAliasing;
  doc["refreshFrequency"] = SETTINGS.refreshFrequency;
  doc["readerRefreshMode"] = SETTINGS.readerRefreshMode;
  doc["readerImageGrayscale"] = SETTINGS.readerImageGrayscale;
  doc["readerSmartRefreshOnImages"] = SETTINGS.readerSmartRefreshOnImages;
  doc["statusBar"] = SETTINGS.statusBar;
  doc["statusBarLeft"] = SETTINGS.statusBarLeft;
  doc["statusBarInnerLeft"] = SETTINGS.statusBarInnerLeft;
  doc["statusBarMiddle"] = SETTINGS.statusBarMiddle;
  doc["statusBarInnerRight"] = SETTINGS.statusBarInnerRight;
  doc["statusBarRight"] = SETTINGS.statusBarRight;

  doc["frontButtonLayout"] = SETTINGS.frontButtonLayout;
  doc["shortPwrBtn"] = SETTINGS.shortPwrBtn;
  doc["powerWakeGuard"] = SETTINGS.powerWakeGuard;
  doc["mainMenuNav"] = SETTINGS.mainMenuNav;

  doc["sleepTimeout"] = SETTINGS.sleepTimeout;
  doc["useLibraryIndex"] = SETTINGS.useLibraryIndex;
  doc["bootSetting"] = SETTINGS.bootSetting;

  doc["refreshOnLoadRecent"] = SETTINGS.refreshOnLoadRecent;
  doc["refreshOnLoadLibrary"] = SETTINGS.refreshOnLoadLibrary;
  doc["refreshOnLoadSettings"] = SETTINGS.refreshOnLoadSettings;
  doc["refreshOnLoadSync"] = SETTINGS.refreshOnLoadSync;
  doc["refreshOnLoadStatistics"] = SETTINGS.refreshOnLoadStatistics;
  doc["pageAutoTurnSeconds"] = SETTINGS.pageAutoTurnSeconds;
  doc["bitmapRoundedCorners"] = SETTINGS.bitmapRoundedCorners;
  doc["sunlightFadingFix"] = SETTINGS.sunlightFadingFix;
  doc["antiGhostingExperimental"] = SETTINGS.antiGhostingExperimental;
  doc["x3ReinforceReader"] = SETTINGS.x3ReinforceReader;
  doc["x3ReinforceUi"] = SETTINGS.x3ReinforceUi;
  doc["x3ReinforceThumbnails"] = SETTINGS.x3ReinforceThumbnails;
  doc["x3ReinforcePeriodicClean"] = SETTINGS.x3ReinforcePeriodicClean;
  doc["x3ReinforceCleanInterval"] = SETTINGS.x3ReinforceCleanInterval;
  doc["opdsServerUrl"] = SETTINGS.opdsServerUrl;
  doc["opdsUsername"] = SETTINGS.opdsUsername;
  doc["opdsPasswordSet"] = strlen(SETTINGS.opdsPassword) > 0;
  doc["newsRepoUrl"] = SETTINGS.newsRepoUrl;
  doc["newsAutoDownload"] = SETTINGS.newsAutoDownload;
  doc["newsDownloadHour"] = SETTINGS.newsDownloadHour;

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void LocalServer::handleSettingsUpdate() const {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);

  if (error) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }

  bool changed = false;
  const bool clockAvailable = clockSettingsAvailable();

  for (JsonPair kv : doc.as<JsonObject>()) {
    const char* key = kv.key().c_str();
    int value = kv.value().as<int>();

    if (strcmp(key, "sleepScreen") == 0) {
      uint8_t v = static_cast<uint8_t>(value);
      if (v >= SystemSetting::SLEEP_SCREEN_MODE_COUNT || (!clockAvailable && v == SystemSetting::DATETIME)) {
        v = SystemSetting::LIGHT;
      }
      SETTINGS.sleepScreen = v;
      changed = true;
    } else if (strcmp(key, "sleepScreenCoverMode") == 0) {
      SETTINGS.sleepScreenCoverMode = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "sleepScreenCoverFilter") == 0) {
      SETTINGS.sleepScreenCoverFilter = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "sleepScreenCoverGrayscale") == 0) {
      SETTINGS.sleepImageQuality = (value >= 0 && value < SystemSetting::SLEEP_IMAGE_QUALITY_COUNT)
                                       ? static_cast<uint8_t>(value)
                                       : SystemSetting::SLEEP_IMAGE_LOW;
      changed = true;
    } else if (strcmp(key, "sleepImageQuality") == 0) {
      SETTINGS.sleepImageQuality = (value >= 0 && value < SystemSetting::SLEEP_IMAGE_QUALITY_COUNT)
                                       ? static_cast<uint8_t>(value)
                                       : SystemSetting::SLEEP_IMAGE_LOW;
      changed = true;
    } else if (strcmp(key, "sleepImageTwoBit") == 0) {
      SETTINGS.sleepImageQuality = (uint8_t)value ? SystemSetting::SLEEP_IMAGE_MEDIUM : SystemSetting::SLEEP_IMAGE_LOW;
      changed = true;
    } else if (strcmp(key, "sleepCustomBmp") == 0) {
      if (kv.value().isNull()) {
        SETTINGS.setSleepCustomBmpFromInput(nullptr);
      } else {
        SETTINGS.setSleepCustomBmpFromInput(kv.value().as<const char*>());
      }
      changed = true;
    } else if (strcmp(key, "sleepImageRotationEnabled") == 0) {
      SETTINGS.sleepImageRotationEnabled = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "sleepImageRotationMinutes") == 0) {
      int v = static_cast<int>(value);
      if (v < 0) v = 0;
      if (v > 120) v = 120;
      if (v != 0) {
        v = ((v + 2) / 5) * 5;
        if (v < 5) v = 5;
      }
      SETTINGS.sleepImageRotationMinutes = static_cast<uint8_t>(v);
      changed = true;
    } else if (strcmp(key, "sleepImagePowerDoublePress") == 0) {
      SETTINGS.sleepImagePowerDoublePress = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "sleepImagePowerGestureWindow") == 0) {
      SETTINGS.sleepImagePowerGestureWindow = value >= 0 && value <= 17 ? static_cast<uint8_t>(value) : 0;
      changed = true;
    } else if (strcmp(key, "sleepImagePowerFirstPressMin") == 0) {
      SETTINGS.sleepImagePowerFirstPressMin = value >= 0 && value <= 15 ? static_cast<uint8_t>(value) : 0;
      changed = true;
    } else if (strcmp(key, "sleepImagePowerSecondPressMax") == 0) {
      SETTINGS.sleepImagePowerSecondPressMax = value >= 0 && value <= 9 ? static_cast<uint8_t>(value) : 2;
      changed = true;
    } else if (strcmp(key, "persistentSleepLogs") == 0) {
      SETTINGS.persistentSleepLogs = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "sleepImageAdvanceButton") == 0) {
      SETTINGS.sleepImagePowerDoublePress = value == 7 ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "powerWakeGuard") == 0) {
      SETTINGS.powerWakeGuard = (value >= 0 && value < SystemSetting::POWER_WAKE_GUARD_COUNT)
                                    ? static_cast<uint8_t>(value)
                                    : SystemSetting::POWER_WAKE_GUARD_OFF;
      changed = true;
    } else if (clockAvailable && strcmp(key, "sleepClockStyle") == 0) {
      uint8_t v = static_cast<uint8_t>(value);
      if (v >= SystemSetting::SLEEP_CLOCK_STYLE_COUNT) v = SystemSetting::CLOCK_CENTERED_DATE;
      SETTINGS.sleepClockStyle = v;
      changed = true;
    } else if (clockAvailable && strcmp(key, "sleepClockTimeFormat") == 0) {
      uint8_t v = static_cast<uint8_t>(value);
      if (v >= SystemSetting::CLOCK_TIME_FORMAT_COUNT) v = SystemSetting::CLOCK_24_HOUR;
      SETTINGS.sleepClockTimeFormat = v;
      changed = true;
    } else if (clockAvailable && strcmp(key, "timeZoneQuarterOffset") == 0) {
      int v = static_cast<int>(value);
      if (v < 0) v = 0;
      if (v > 104) v = 104;
      SETTINGS.timeZoneQuarterOffset = static_cast<uint8_t>(v);
      changed = true;
    } else if (strcmp(key, "hideBatteryPercentage") == 0) {
      SETTINGS.hideBatteryPercentage = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "uiTheme") == 0) {
      const uint8_t v = static_cast<uint8_t>(value);
      SETTINGS.uiTheme = v < SystemSetting::UI_THEME_COUNT ? v : SystemSetting::UI_THEME_CLASSIC;
      changed = true;
    } else if (strcmp(key, "recentLibraryMode") == 0) {
      SETTINGS.recentLibraryMode = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "libraryMode") == 0) {
      uint8_t v = static_cast<uint8_t>(value);
      if (v >= SystemSetting::LIBRARY_MODE_COUNT) v = SystemSetting::LIBRARY_LIST;
      SETTINGS.libraryMode = v;
      changed = true;
    } else if (strcmp(key, "recentVisibleCount") == 0) {
      int v = static_cast<int>(value);
      if (v < 1) v = 1;
      if (v > 8) v = 8;
      SETTINGS.recentVisibleCount = static_cast<uint8_t>(v);
      changed = true;
    } else if (strcmp(key, "librarySortEnabled") == 0) {
      SETTINGS.librarySortEnabled = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "librarySortMode") == 0) {
      int v = static_cast<int>(value);
      if (v < 0) v = 0;
      if (v > 6) v = 0;
      SETTINGS.librarySortMode = static_cast<uint8_t>(v);
      changed = true;
    } else if (strcmp(key, "fontFamily") == 0) {
      SETTINGS.fontFamily = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "fontSize") == 0) {
      SETTINGS.fontSize = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "lineHeight") == 0) {
      uint8_t v = (uint8_t)value;
      SETTINGS.lineHeight = (v < 10 || v > 200) ? 100 : v;
      changed = true;
    } else if (strcmp(key, "textSpace") == 0) {
      uint8_t v = (uint8_t)value;
      SETTINGS.textSpace = (v < 10 || v > 200) ? 100 : v;
      changed = true;
    } else if (strcmp(key, "screenMargin") == 0) {
      SETTINGS.screenMargin = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "paragraphAlignment") == 0) {
      SETTINGS.paragraphAlignment = (uint8_t)value;
      if (SETTINGS.paragraphAlignment >= SystemSetting::PARAGRAPH_ALIGNMENT_COUNT) {
        SETTINGS.paragraphAlignment = SystemSetting::JUSTIFIED;
      }
      changed = true;
    } else if (strcmp(key, "extraParagraphSpacing") == 0) {
      SETTINGS.extraParagraphSpacing = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "paragraphCssIndentEnabled") == 0) {
      SETTINGS.paragraphCssIndentEnabled = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "orientation") == 0) {
      SETTINGS.orientation = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "hyphenationEnabled") == 0) {
      SETTINGS.hyphenationEnabled = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "bionicReadingEnabled") == 0) {
      SETTINGS.bionicReadingEnabled = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "readerDirectionMapping") == 0) {
      SETTINGS.readerDirectionMapping = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "readerMenuButton") == 0) {
      uint8_t v = (uint8_t)value;
      if (v >= SystemSetting::READER_MENU_BUTTON_COUNT) {
        v = SystemSetting::MENU_UP;
      }
      SETTINGS.readerMenuButton = v;
      changed = true;
    } else if (strcmp(key, "longPressChapterSkip") == 0) {
      const int v = static_cast<int>(value);
      SETTINGS.longPressChapterSkip =
          (v < 0) ? 0
                  : (v > SystemSetting::LONG_PRESS_PAGE_SKIP_5 ? SystemSetting::LONG_PRESS_PAGE_SKIP_5 : (uint8_t)v);
      changed = true;
    } else if (strcmp(key, "readerShortPwrBtn") == 0) {
      SETTINGS.readerShortPwrBtn = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "shakePageTurn") == 0) {
      const int motionMode = static_cast<int>(value);
      SETTINGS.shakePageTurn = static_cast<uint8_t>(motionMode < 0 ? 0 : motionMode > 2 ? 2 : motionMode);
      changed = true;
    } else if (strcmp(key, "shakePageTurnSensitivity") == 0) {
      const int sensitivity = static_cast<int>(value);
      SETTINGS.shakePageTurnSensitivity = static_cast<uint8_t>(sensitivity < 0 ? 0 : sensitivity > 2 ? 2 : sensitivity);
      changed = true;
    } else if (strcmp(key, "textAntiAliasing") == 0) {
      SETTINGS.textAntiAliasing = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "refreshFrequency") == 0) {
      SETTINGS.refreshFrequency = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "readerRefreshMode") == 0) {
      SETTINGS.readerRefreshMode = (value >= 0 && value < SystemSetting::READER_REFRESH_MODE_COUNT)
                                       ? (uint8_t)value
                                       : SystemSetting::READER_REFRESH_AUTO;
      changed = true;
    } else if (strcmp(key, "readerImageGrayscale") == 0) {
      SETTINGS.readerImageGrayscale = (value >= 0 && value < SystemSetting::READER_IMAGE_QUALITY_COUNT)
                                          ? (uint8_t)value
                                          : SystemSetting::READER_IMAGE_LOW;
      changed = true;
    } else if (strcmp(key, "readerSmartRefreshOnImages") == 0) {
      SETTINGS.readerSmartRefreshOnImages = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "statusBar") == 0) {
      SETTINGS.statusBar = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "statusBarLeft") == 0) {
      SETTINGS.statusBarLeft = value >= 0 && value < SystemSetting::STATUS_BAR_ITEM_COUNT ? (uint8_t)value : 0;
      changed = true;
    } else if (strcmp(key, "statusBarInnerLeft") == 0) {
      SETTINGS.statusBarInnerLeft = value >= 0 && value < SystemSetting::STATUS_BAR_ITEM_COUNT ? (uint8_t)value : 0;
      changed = true;
    } else if (strcmp(key, "statusBarMiddle") == 0) {
      SETTINGS.statusBarMiddle = value >= 0 && value < SystemSetting::STATUS_BAR_ITEM_COUNT ? (uint8_t)value : 0;
      changed = true;
    } else if (strcmp(key, "statusBarInnerRight") == 0) {
      SETTINGS.statusBarInnerRight = value >= 0 && value < SystemSetting::STATUS_BAR_ITEM_COUNT ? (uint8_t)value : 0;
      changed = true;
    } else if (strcmp(key, "statusBarRight") == 0) {
      SETTINGS.statusBarRight = value >= 0 && value < SystemSetting::STATUS_BAR_ITEM_COUNT ? (uint8_t)value : 0;
      changed = true;
    } else if (strcmp(key, "showBottomBarClock") == 0) {
      SETTINGS.showBottomBarClock = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "frontButtonLayout") == 0) {
      SETTINGS.frontButtonLayout = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "shortPwrBtn") == 0) {
      SETTINGS.shortPwrBtn = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "mainMenuNav") == 0) {
      SETTINGS.mainMenuNav = (uint8_t)value ? SystemSetting::MAIN_MENU_NAV_SIDE : SystemSetting::MAIN_MENU_NAV_FRONT;
      changed = true;
    } else if (strcmp(key, "sleepTimeout") == 0) {
      SETTINGS.sleepTimeout = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "useLibraryIndex") == 0) {
      SETTINGS.useLibraryIndex = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "libraryShelfEnabled") == 0) {
      SETTINGS.libraryShelfEnabled = (uint8_t)value ? 1 : 0;
      if (!SETTINGS.libraryShelfEnabled && SETTINGS.libraryViewMode == SystemSetting::LIBRARY_VIEW_SHELF) {
        SETTINGS.libraryViewMode = SystemSetting::LIBRARY_VIEW_FOLDERS;
      }
      changed = true;
    } else if (strcmp(key, "bootSetting") == 0) {
      SETTINGS.bootSetting = (uint8_t)value;
      changed = true;
    } else if (strcmp(key, "refreshOnLoadRecent") == 0) {
      SETTINGS.refreshOnLoadRecent = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "refreshOnLoadLibrary") == 0) {
      SETTINGS.refreshOnLoadLibrary = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "refreshOnLoadSettings") == 0) {
      SETTINGS.refreshOnLoadSettings = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "refreshOnLoadSync") == 0) {
      SETTINGS.refreshOnLoadSync = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "refreshOnLoadStatistics") == 0) {
      SETTINGS.refreshOnLoadStatistics = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "pageAutoTurnSeconds") == 0) {
      int v = static_cast<int>(value);
      if (v < 0) v = 0;
      if (v > 180) v = 180;
      v = (v / 10) * 10;
      SETTINGS.pageAutoTurnSeconds = static_cast<uint8_t>(v);
      changed = true;
    } else if (strcmp(key, "bitmapRoundedCorners") == 0) {
      int cornerStyle = static_cast<int>(value);
      if (cornerStyle < 0) cornerStyle = 0;
      if (cornerStyle > 2) cornerStyle = 2;
      SETTINGS.bitmapRoundedCorners = static_cast<uint8_t>(cornerStyle);
      changed = true;
    } else if (strcmp(key, "sunlightFadingFix") == 0) {
      SETTINGS.sunlightFadingFix = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "antiGhostingExperimental") == 0) {
      SETTINGS.antiGhostingExperimental = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "x3ReinforceReader") == 0) {
      SETTINGS.x3ReinforceReader = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "x3ReinforceUi") == 0) {
      SETTINGS.x3ReinforceUi = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "x3ReinforceThumbnails") == 0) {
      SETTINGS.x3ReinforceThumbnails = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "x3ReinforcePeriodicClean") == 0) {
      SETTINGS.x3ReinforcePeriodicClean = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "x3ReinforceCleanInterval") == 0) {
      int interval = static_cast<int>(value);
      if (interval < 0) interval = 0;
      if (interval >= SystemSetting::X3_REINFORCE_CLEAN_INTERVAL_COUNT) {
        interval = SystemSetting::X3_REINFORCE_CLEAN_30;
      }
      SETTINGS.x3ReinforceCleanInterval = static_cast<uint8_t>(interval);
      changed = true;
    } else if (strcmp(key, "opdsServerUrl") == 0) {
      copySettingString(SETTINGS.opdsServerUrl, sizeof(SETTINGS.opdsServerUrl), kv.value().as<const char*>());
      changed = true;
    } else if (strcmp(key, "opdsUsername") == 0) {
      copySettingString(SETTINGS.opdsUsername, sizeof(SETTINGS.opdsUsername), kv.value().as<const char*>());
      changed = true;
    } else if (strcmp(key, "opdsPassword") == 0) {
      copySettingString(SETTINGS.opdsPassword, sizeof(SETTINGS.opdsPassword), kv.value().as<const char*>());
      changed = true;
    } else if (strcmp(key, "newsRepoUrl") == 0) {
      copySettingString(SETTINGS.newsRepoUrl, sizeof(SETTINGS.newsRepoUrl), kv.value().as<const char*>());
      changed = true;
    } else if (strcmp(key, "newsAutoDownload") == 0) {
      SETTINGS.newsAutoDownload = (uint8_t)value ? 1 : 0;
      changed = true;
    } else if (strcmp(key, "newsDownloadHour") == 0) {
      uint8_t h = (uint8_t)value;
      if (h <= 23) SETTINGS.newsDownloadHour = h;
      changed = true;
    }
  }

  if (changed) {
    SETTINGS.saveToFile();
    Serial.printf("[%lu] [WEB] Settings updated and saved\n", millis());
  }

  server->send(200, "application/json", "{\"status\":\"ok\"}");
}

void LocalServer::handleSleepWakeTraceGet() const {
#ifndef INX_SIMULATOR_WEB_ONLY
  const HalGPIO::SleepWakeTraceSnapshot snapshot = HalGPIO::getSleepWakeTrace();
  JsonDocument doc;
  doc["count"] = snapshot.count;
#ifndef SIMULATOR
  const HalGPIO::SleepWakeTraceSnapshot persistedSnapshot = loadSleepWakeTraceCheckpoint();
  doc["persistedCount"] = persistedSnapshot.count;
  JsonArray persistedEvents = doc["persistedEvents"].to<JsonArray>();
  for (uint8_t i = 0; i < persistedSnapshot.count; ++i) {
    const HalGPIO::SleepWakeTraceEntry& entry = persistedSnapshot.entries[i];
    JsonObject item = persistedEvents.add<JsonObject>();
    item["seq"] = entry.sequence;
    item["tick"] = entry.rtcTickLow;
    item["event"] = sleepWakeTraceEventName(entry.event);
    item["arg0"] = entry.arg0;
    item["arg1"] = entry.arg1;
  }
#else
  doc["persistedCount"] = 0;
  doc["persistedEvents"].to<JsonArray>();
#endif
  JsonArray events = doc["events"].to<JsonArray>();
  for (uint8_t i = 0; i < snapshot.count; ++i) {
    const HalGPIO::SleepWakeTraceEntry& entry = snapshot.entries[i];
    JsonObject item = events.add<JsonObject>();
    item["seq"] = entry.sequence;
    item["tick"] = entry.rtcTickLow;
    item["event"] = sleepWakeTraceEventName(entry.event);
    item["arg0"] = entry.arg0;
    item["arg1"] = entry.arg1;
  }
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
#else
  server->send(200, "application/json", "{\"count\":0,\"persistedCount\":0,\"persistedEvents\":[],\"events\":[]}");
#endif
}

void LocalServer::handleSleepWakeTraceClear() const {
#ifndef INX_SIMULATOR_WEB_ONLY
  HalGPIO::clearSleepWakeTrace();
#ifndef SIMULATOR
  clearSleepWakeTraceCheckpoint();
#endif
#endif
  server->send(200, "application/json", "{\"status\":\"ok\"}");
}

void LocalServer::handleWallpapersGet() const {
  JsonDocument doc;
  doc["random"] = SETTINGS.sleepCustomBmp[0] == '\0';
  doc["selected"] = SETTINGS.sleepCustomBmp;
  JsonArray itemsJson = doc["items"].to<JsonArray>();

  const std::vector<WallpaperInfo> items = collectWallpapers();
  for (const auto& item : items) {
    JsonObject obj = itemsJson.add<JsonObject>();
    obj["path"] = item.path;
    obj["label"] = item.label;
    obj["shuffle"] = isSleepImageShuffleEnabled(item.path.c_str());
    obj["selected"] = wallpaperMatchesCurrentSelection(item.path);
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void LocalServer::handleWallpaperImageGet() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  const String path = normalizeWallpaperPath(server->arg("path"));
  if (!isWallpaperPathAllowed(path)) {
    server->send(403, "text/plain", "Invalid wallpaper path");
    return;
  }
  if (!SdMan.exists(path.c_str())) {
    server->send(404, "text/plain", "Wallpaper not found");
    return;
  }

  FsFile file = SdMan.open(path.c_str(), O_READ);
  if (!file) {
    server->send(500, "text/plain", "Failed to open wallpaper");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  server->setContentLength(file.size());
  server->send(200, wallpaperMimeType(path).c_str(), "");

  WiFiClient client = server->client();
  client.write(file);
  file.close();
}

void LocalServer::handleWallpaperShufflePost() const {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, server->arg("plain"));
  if (error) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }

  bool settingsChanged = false;

  if (doc["path"].is<const char*>() && !doc["shuffle"].isNull()) {
    const String path = normalizeWallpaperPath(doc["path"].as<const char*>());
    if (!isWallpaperPathAllowed(path) || !SdMan.exists(path.c_str())) {
      server->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_path\"}");
      return;
    }

    const bool enabled = doc["shuffle"].as<bool>();
    setSleepImageShuffleEnabled(path.c_str(), enabled);
    if (!enabled) {
      const std::vector<WallpaperInfo> items = collectWallpapers();
      if (enabledWallpaperCount(items) == 0) {
        setSleepImageShuffleEnabled(path.c_str(), true);
        server->send(409, "application/json", "{\"ok\":false,\"error\":\"last_enabled\"}");
        return;
      }
    }
  }

  if (!doc["random"].isNull()) {
    if (doc["random"].as<bool>()) {
      SETTINGS.setSleepCustomBmpFromInput("");
      settingsChanged = true;
    } else if (doc["selected"].is<const char*>()) {
      const String selected = normalizeWallpaperPath(doc["selected"].as<const char*>());
      if (!isWallpaperPathAllowed(selected) || !SdMan.exists(selected.c_str())) {
        server->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_selected\"}");
        return;
      }
      SETTINGS.setSleepCustomBmpFromInput(selected.c_str());
      settingsChanged = true;
    }
  } else if (doc["selected"].is<const char*>()) {
    const String selected = normalizeWallpaperPath(doc["selected"].as<const char*>());
    if (!isWallpaperPathAllowed(selected) || !SdMan.exists(selected.c_str())) {
      server->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_selected\"}");
      return;
    }
    SETTINGS.setSleepCustomBmpFromInput(selected.c_str());
    settingsChanged = true;
  }

  if (settingsChanged) {
    SETTINGS.saveToFile();
  }

  server->send(200, "application/json", "{\"ok\":true}");
}

void LocalServer::handleWifiGet() const {
  JsonDocument doc;
  const auto& creds = WIFI_STORE.getCredentials();
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& cred : creds) {
    JsonObject obj = arr.add<JsonObject>();
    obj["ssid"] = cred.ssid;
  }
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void LocalServer::handleWifiPost() const {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON");
    return;
  }

  JsonDocument doc;
  deserializeJson(doc, server->arg("plain"));
  String ssid = doc["ssid"];
  String password = doc["password"] | "";

  if (WIFI_STORE.addCredential(ssid.c_str(), password.c_str())) {
    WIFI_STORE.saveToFile();
    server->send(200, "application/json", "{\"status\":\"ok\"}");
  } else {
    server->send(500, "text/plain", "Failed to save");
  }
}

void LocalServer::handleWifiDelete() const {
  String uri = server->uri();
  int lastSlash = uri.lastIndexOf('/');
  String ssid = uri.substring(lastSlash + 1);
  ssid.replace("%20", " ");

  if (WIFI_STORE.removeCredential(ssid.c_str())) {
    WIFI_STORE.saveToFile();
    server->send(200, "application/json", "{\"status\":\"ok\"}");
  } else {
    server->send(404, "text/plain", "Not found");
  }
}

void LocalServer::handleKOReaderGet() const {
  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["serverUrl"] = KOREADER_STORE.getServerUrl();
  doc["matchMethod"] = (int)KOREADER_STORE.getMatchMethod();
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void LocalServer::handleKOReaderPost() const {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON");
    return;
  }

  JsonDocument doc;
  deserializeJson(doc, server->arg("plain"));

  String username = doc["username"] | "";
  String password = doc["password"].is<const char*>() ? (doc["password"] | "") : KOREADER_STORE.getPassword().c_str();
  String serverUrl = doc["serverUrl"] | "";
  int matchMethod = doc["matchMethod"] | 0;

  KOREADER_STORE.setCredentials(username.c_str(), password.c_str());
  KOREADER_STORE.setServerUrl(serverUrl.c_str());
  KOREADER_STORE.setMatchMethod((DocumentMatchMethod)matchMethod);
  KOREADER_STORE.saveToFile();

  server->send(200, "application/json", "{\"status\":\"ok\"}");
}

void LocalServer::handleFontsRescan() const {
#ifdef INX_SIMULATOR_WEB_ONLY
  server->send(501, "application/json", "{\"ok\":false,\"error\":\"unavailable_in_simulator\"}");
#else
  if (!SdMan.ready()) {
    server->send(503, "application/json", "{\"ok\":false,\"error\":\"sd_unavailable\"}");
    return;
  }
  const bool ok = FontManager::scanSDFonts("/fonts", true);
  if (ok) {
    server->send(200, "application/json", "{\"ok\":true}");
  } else {
    server->send(500, "application/json", "{\"ok\":false,\"error\":\"scan_failed\"}");
  }
#endif
}

#ifndef INX_SIMULATOR_WEB_ONLY
void LocalServer::handleOpdsGet() const {
  JsonDocument doc;
  const auto& servers = OPDS_STORE.getAllServers();
  JsonArray arr = doc.to<JsonArray>();
  for (const auto& srv : servers) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = srv.name;
    obj["url"] = srv.url;
    obj["username"] = srv.username;
  }
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void LocalServer::handleOpdsPost() const {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON");
    return;
  }

  JsonDocument doc;
  deserializeJson(doc, server->arg("plain"));
  String name = doc["name"];
  String url = doc["url"];
  String username = doc["username"] | "";
  String password = doc["password"] | "";

  if (name.length() == 0 || url.length() == 0) {
    server->send(400, "text/plain", "Name and URL are required");
    return;
  }

  if (OPDS_STORE.addServer(name.c_str(), url.c_str(), username.c_str(), password.c_str())) {
    server->send(200, "application/json", "{\"status\":\"ok\"}");
  } else {
    server->send(500, "text/plain", "Failed to save");
  }
}

void LocalServer::handleOpdsDelete() const {
  String uri = server->uri();
  int lastSlash = uri.lastIndexOf('/');
  String name = uri.substring(lastSlash + 1);
  name.replace("%20", " ");

  if (OPDS_STORE.removeServer(name.c_str())) {
    server->send(200, "application/json", "{\"status\":\"ok\"}");
  } else {
    server->send(404, "text/plain", "Not found");
  }
}
#endif
