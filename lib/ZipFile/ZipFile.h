#pragma once

/**
 * @file ZipFile.h
 * @brief Public interface and types for ZipFile.
 */

#include <SdFat.h>

#include <string>
#include <unordered_map>
#include <vector>

class ZipFile {
 public:
  struct FileStatSlim {
    uint16_t method;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint32_t localHeaderOffset;
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint16_t totalEntries;
    bool isSet;
  };

  struct SizeTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
  };

  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  const std::string& filePath;
  FsFile file;
  ZipDetails zipDetails = {0, 0, false};
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  uint32_t lastCentralDirPos = 0;
  bool lastCentralDirPosValid = false;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();

 public:
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  ~ZipFile() = default;

  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  bool getInflatedFileSize(const char* filename, size_t* size);

  int fillUncompressedSizes(std::vector<SizeTarget>& targets, std::vector<uint32_t>& sizes);

  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize);

  /**
   * Holds the inflate working set — a 32 KB LZ window and the decompressor — for as long as it
   * is in scope, so every deflated entry read meanwhile can share it.
   *
   * The window has to be 32 KB and it has to be contiguous. Allocating it per entry means asking
   * for it in the middle of a chapter parse, which is exactly when the heap is at its most
   * fragmented — an image would fail to extract while free heap still read over 100 KB, and the
   * chapter cached without it. Claiming it before the parse begins moves the one hard allocation
   * to the moment it is easy, and costs nothing at all for books with no deflated images.
   *
   * Scoped rather than permanent: 43 KB is worth holding for a parse, not for the reader's life.
   * Nesting is counted, so an inner scope does not free what an outer one is still using.
   * Deliberately not thread-safe — parsing happens on one task.
   */
  class InflateScratch {
   public:
    InflateScratch();
    ~InflateScratch();
    InflateScratch(const InflateScratch&) = delete;
    InflateScratch& operator=(const InflateScratch&) = delete;

    /** False when the scratch could not be claimed; readers then allocate per entry as before. */
    bool valid() const;
  };
};
