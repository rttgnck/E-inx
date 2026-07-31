/**
 * @file SleepImageSelection.cpp
 * @brief Stores unchecked sleep wallpaper paths in a simple newline manifest.
 */

#include "SleepImageSelection.h"

#include <SDCardManager.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {
constexpr char kExclusionsPath[] = "/.system/sleep_image_exclusions.txt";

std::vector<std::string> readExcludedPaths() {
  std::vector<std::string> paths;
  FsFile file;
  if (!SdMan.openFileForRead("SIS", kExclusionsPath, file)) {
    return paths;
  }

  std::string line;
  int ch = 0;
  while ((ch = file.read()) >= 0) {
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      if (!line.empty()) {
        paths.push_back(line);
        line.clear();
      }
      continue;
    }
    line.push_back(static_cast<char>(ch));
  }
  if (!line.empty()) {
    paths.push_back(line);
  }
  file.close();
  return paths;
}

void writeExcludedPaths(const std::vector<std::string>& paths) {
  SdMan.mkdir("/.system");
  FsFile file;
  if (!SdMan.openFileForWrite("SIS", kExclusionsPath, file)) {
    return;
  }
  for (const auto& path : paths) {
    file.write(reinterpret_cast<const uint8_t*>(path.c_str()), path.length());
    const uint8_t newline = '\n';
    file.write(&newline, 1);
  }
  file.close();
}
}  // namespace

bool isSleepImageShuffleEnabled(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  const auto excluded = readExcludedPaths();
  return std::find(excluded.begin(), excluded.end(), path) == excluded.end();
}

void setSleepImageShuffleEnabled(const std::string& path, const bool enabled) {
  if (path.empty()) {
    return;
  }

  auto excluded = readExcludedPaths();
  const auto it = std::find(excluded.begin(), excluded.end(), path);
  if (enabled) {
    if (it != excluded.end()) {
      excluded.erase(it);
      writeExcludedPaths(excluded);
    }
    return;
  }

  if (it == excluded.end()) {
    excluded.push_back(path);
    std::sort(excluded.begin(), excluded.end());
    excluded.erase(std::unique(excluded.begin(), excluded.end()), excluded.end());
    writeExcludedPaths(excluded);
  }
}
