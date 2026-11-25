#pragma once

/**
 * @file LibraryIndexer.h
 * @brief Public interface and types for LibraryIndexer.
 */

#include <Arduino.h>
#include <Epub.h>
#include <SDCardManager.h>
#include <Xtc.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <string>

#include "util/SdIoMutex.h"

class LibraryIndexer {
 public:
  static constexpr const char* kIndexPath = "/.metadata/library/library.idx";
  // indexAll() builds here and renames over kIndexPath only when complete, so a crash or a reader mid-rebuild
  // never sees a truncated index.
  static constexpr const char* kTempIndexPath = "/.metadata/library/library.idx.tmp";
  // Present when books were added, removed or renamed since the last index (web uploads, deletes, renames,
  // Bluetooth transfers). The library pages rebuild on open while it exists.
  static constexpr const char* kStalePath = "/.metadata/library/library.stale";
  // Stack for any task that calls indexAll(): it opens each new EPUB and decodes and re-encodes its cover
  // (generateMissingThumbnail), the same work ThumbnailGeneratorActivity gives 12 KB. 4 KB overflowed.
  static constexpr uint32_t kTaskStackBytes = 12288;

  static bool hasIndex() { return SdMan.exists(kIndexPath); }
  static bool deleteIndex() { return SdMan.remove(kIndexPath); }

  static bool isStale() {
    SdIoMutex::Lock lock;
    return SdMan.exists(kStalePath);
  }

  /** Records that the book files changed, so the next library view rebuilds the index. */
  static void markStale() {
    SdIoMutex::Lock lock;
    if (SdMan.exists(kStalePath)) return;
    SdMan.mkdir("/.metadata/library");
    FsFile marker = SdMan.open(kStalePath, O_WRITE | O_CREAT | O_TRUNC);
    if (marker) marker.close();
  }

  static bool isBookName(const char* name) {
    const char* ext = strrchr(name, '.');
    return ext && (strcasecmp(ext, ".epub") == 0 || strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".md") == 0 ||
                   strcasecmp(ext, ".xtc") == 0 || strcasecmp(ext, ".xtch") == 0);
  }

  static int countBooks(FsFile& dir, int depth = 0) {
    if (depth > 32) return 0;
    int count = 0;
    {
      SdIoMutex::Lock lock;
      dir.rewindDirectory();
    }
    char name[256];
    FsFile file;
    while (true) {
      bool isDir = false;
      {
        SdIoMutex::Lock lock;
        if (!openNextEntry(dir, file)) break;
        file.getName(name, sizeof(name));
        if (shouldSkipEntry(name)) {
          file.close();
          continue;
        }
        isDir = file.isDirectory();
        if (!isDir && isBookName(name)) count++;
      }
      if (isDir) count += countBooks(file, depth + 1);
      releaseEntry(file);
    }
    return count;
  }

  static void indexAll(std::function<void(int, int, const char*)> progressCallback) {
    // One rebuild at a time: a web refresh can still be running when the device's Library page starts its own.
    bool expected = false;
    while (!indexingFlag().compare_exchange_weak(expected, true)) {
      expected = false;
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    indexAllLocked(progressCallback);
    indexingFlag().store(false);
  }

 private:
  static std::atomic<bool>& indexingFlag() {
    static std::atomic<bool> flag{false};
    return flag;
  }

  static void indexAllLocked(const std::function<void(int, int, const char*)>& progressCallback) {
    {
      SdIoMutex::Lock lock;
      if (!SdMan.exists("/.metadata")) SdMan.mkdir("/.metadata");
      if (!SdMan.exists("/.metadata/library")) SdMan.mkdir("/.metadata/library");
      // Cleared before scanning: a book added while this runs marks the index stale again.
      SdMan.remove(kStalePath);
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);

    int totalBooks = 0;
    FsFile root;
    {
      SdIoMutex::Lock lock;
      root = SdMan.open("/");
    }
    if (root) {
      totalBooks = countBooks(root);
      SdIoMutex::Lock lock;
      root.close();
    }

    FsFile idxFile;
    {
      SdIoMutex::Lock lock;
      SdMan.remove(kTempIndexPath);
      idxFile = SdMan.open(kTempIndexPath, O_WRITE | O_CREAT | O_TRUNC);
      if (idxFile) {
        idxFile.write("LIBX", 4);
        uint8_t version = 1;
        idxFile.write(&version, 1);
        root = SdMan.open("/");
      }
    }
    if (!idxFile) {
      markStale();  // try again the next time a library opens
      if (progressCallback) progressCallback(0, 0, "Failed to create index");
      return;
    }

    bool written = static_cast<bool>(root);
    if (root) {
      int currentBook = 0;
      indexDirectory(root, idxFile, currentBook, totalBooks, progressCallback, std::string("/"), 0);
    }

    {
      SdIoMutex::Lock lock;
      if (root) root.close();
      written = idxFile.close() && written;
      // FAT rename will not replace an existing file, so the old index goes first; readers see no index for
      // that instant rather than a half-written one.
      if (written) {
        SdMan.remove(kIndexPath);
        written = SdMan.rename(kTempIndexPath, kIndexPath);
      }
      if (!written) {
        SdMan.remove(kTempIndexPath);
        markStale();  // try again the next time a library opens
      }
    }

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
    Serial.printf("[%lu] [LIBIDX] %s: %d books, stack headroom %u bytes\n", millis(),
                  written ? "Index written" : "Index write failed", totalBooks,
                  static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
#endif
    if (progressCallback) {
      progressCallback(totalBooks, totalBooks,
                       !written ? "Failed to write index" : (totalBooks ? "Indexing complete!" : "No books found"));
    }
  }

  // Closes an entry, then lets a same-priority task waiting on SdIoMutex (the web server loop) run before this
  // one takes the lock again.
  static void releaseEntry(FsFile& file) {
    {
      SdIoMutex::Lock lock;
      file.close();
    }
    yield();
  }

  static bool openNextEntry(FsFile& dir, FsFile& file) {
#ifdef SIMULATOR
    file = dir.openNextFile();
    return static_cast<bool>(file);
#else
    return file.openNext(&dir, O_RDONLY);
#endif
  }

  static bool shouldSkipEntry(const char* name) {
    return name == nullptr || name[0] == '.' || strcmp(name, ".metadata") == 0 || strcasecmp(name, "sleep") == 0 ||
           strcasecmp(name, "fonts") == 0;
  }

  // Holds SdIoMutex only around each step's own SD work, never across the recursion or the progress
  // callback, so a web request can run between books.
  static void indexDirectory(FsFile& dir, FsFile& idxFile, int& currentBook, int totalBooks,
                             const std::function<void(int, int, const char*)>& progressCallback,
                             const std::string& currentPath, int depth) {
    if (depth > 32) return;

    char name[256];
    uint16_t entryCount = 0;
    uint32_t countPos = 0;
    {
      SdIoMutex::Lock lock;
      dir.rewindDirectory();

      uint8_t dirMarker = 0xFF;
      idxFile.write(&dirMarker, 1);
      uint16_t pathLen = (uint16_t)currentPath.length();
      idxFile.write(&pathLen, sizeof(pathLen));
      idxFile.write(currentPath.c_str(), pathLen);

      countPos = idxFile.position();
      idxFile.write(&entryCount, sizeof(entryCount));
    }

    FsFile file;
    while (true) {
      bool isDir = false;
      bool isBook = false;
      std::string thisItemPath;
      {
        SdIoMutex::Lock lock;
        if (!openNextEntry(dir, file)) break;
        file.getName(name, sizeof(name));
        if (shouldSkipEntry(name)) {
          file.close();
          continue;
        }

        if (currentPath == "/")
          thisItemPath = "/" + std::string(name);
        else
          thisItemPath = currentPath + "/" + std::string(name);

        isDir = file.isDirectory();
        isBook = !isDir && isBookName(name);
        if (isBook) {
          uint8_t bookMarker = 0x01;
          idxFile.write(&bookMarker, 1);

          uint16_t pLen = (uint16_t)thisItemPath.length();
          idxFile.write(&pLen, sizeof(pLen));
          idxFile.write(thisItemPath.c_str(), pLen);

          uint8_t nLen = (uint8_t)strlen(name);
          idxFile.write(&nLen, sizeof(nLen));
          idxFile.write(name, nLen);

          std::string displayName = cleanFilename(name);
          uint8_t dLen = (uint8_t)displayName.length();
          idxFile.write(&dLen, sizeof(dLen));
          idxFile.write(displayName.c_str(), dLen);

          std::string folderName = extractRawFolderName(currentPath.c_str());
          uint8_t fLen = (uint8_t)folderName.length();
          idxFile.write(&fLen, sizeof(fLen));
          idxFile.write(folderName.c_str(), fLen);
        }
      }

      if (isDir) {
        indexDirectory(file, idxFile, currentBook, totalBooks, progressCallback, thisItemPath, depth + 1);
        entryCount++;
      } else if (isBook) {
        currentBook++;
        entryCount++;
        generateMissingThumbnail(thisItemPath);
        if (progressCallback) progressCallback(currentBook, totalBooks, thisItemPath.c_str());
      }
      releaseEntry(file);
    }

    SdIoMutex::Lock lock;
    uint32_t endPos = idxFile.position();
    idxFile.seek(countPos);
    idxFile.write(&entryCount, sizeof(entryCount));
    idxFile.seek(endPos);

    uint8_t endMarker = 0xFE;
    idxFile.write(&endMarker, 1);
  }

  static std::string extractRawFolderName(const char* path) {
    std::string result = path;
    if (result.empty() || result == "/") return "Root";
    if (result.back() == '/') result.pop_back();
    size_t lastSlash = result.find_last_of('/');
    if (lastSlash != std::string::npos) {
      std::string folderName = result.substr(lastSlash + 1);
      return folderName.empty() ? "Root" : folderName;
    }
    return result;
  }

  static std::string cleanFilename(const char* name) {
    std::string result = name;
    size_t dot = result.find_last_of('.');
    if (dot != std::string::npos) {
      result.resize(dot);
    }
    std::replace(result.begin(), result.end(), '_', ' ');
    std::replace(result.begin(), result.end(), '-', ' ');
    return result;
  }

  static void generateMissingThumbnail(const std::string& path) {
    SdIoMutex::Lock lock;
    const char* ext = strrchr(path.c_str(), '.');
    if (!ext) {
      return;
    }
    if (strcasecmp(ext, ".epub") == 0) {
      Epub epub(path, "/.metadata/epub");
      const std::string thumbJpegPath = epub.getThumbJpegPath();
      const std::string thumbBmpPath = epub.getThumbBmpPath();
      if (SdMan.exists(thumbJpegPath.c_str()) || SdMan.exists(thumbBmpPath.c_str())) {
        return;
      }
      if (!epub.load() || !epub.generateThumbBmp()) {
        Serial.printf("[%lu] [LIBIDX] Thumbnail generation failed: %s\n", millis(), path.c_str());
      }
      return;
    }
    if (strcasecmp(ext, ".xtc") == 0) {
      Xtc xtc(path, "/.metadata/xtc");
      const std::string thumbBmpPath = xtc.getThumbBmpPath();
      if (SdMan.exists(thumbBmpPath.c_str())) {
        return;
      }
      if (!xtc.load() || !xtc.generateThumbBmp()) {
        Serial.printf("[%lu] [LIBIDX] Thumbnail generation failed: %s\n", millis(), path.c_str());
      }
    }
  }
};
