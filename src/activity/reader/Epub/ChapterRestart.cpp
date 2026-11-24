/**
 * @file ChapterRestart.cpp
 * @brief Definitions for ChapterRestart.
 */

#include "ChapterRestart.h"

#include <Arduino.h>

#ifndef SIMULATOR
#include <esp_attr.h>
#include <esp_system.h>
#endif

namespace {

constexpr uint32_t kMagic = 0x43485254;  // "CHRT": the fields below were written by this code, not left by power-on.

// RTC_NOINIT_ATTR, not RTC_DATA_ATTR: the latter is re-initialised by a software reset, which is the one
// event these have to survive.
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcChapterKey;
RTC_NOINIT_ATTR uint8_t rtcResumePending;

uint32_t chapterKey(const std::string& bookPath, const int spineIndex) {
  uint32_t hash = 2166136261u;
  for (const char c : bookPath) {
    hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  }
  return (hash ^ static_cast<uint32_t>(spineIndex)) * 16777619u;
}

}  // namespace

namespace ChapterRestart {

bool alreadyTried(const std::string& bookPath, const int spineIndex) {
  return rtcMagic == kMagic && rtcChapterKey == chapterKey(bookPath, spineIndex);
}

void restartInto(const std::string& bookPath, const int spineIndex) {
  rtcMagic = kMagic;
  rtcChapterKey = chapterKey(bookPath, spineIndex);
  rtcResumePending = 1;
  Serial.printf("[%lu] [CHR] restarting to open spine=%d of %s on a fresh heap\n", millis(), spineIndex,
                bookPath.c_str());
  Serial.flush();
  delay(50);
#ifdef SIMULATOR
  abort();
#else
  esp_restart();
#endif
}

void clear() { rtcMagic = 0; }

bool consumeResumeRequest() {
  if (rtcMagic != kMagic || rtcResumePending != 1) {
    return false;
  }
  rtcResumePending = 0;
  return true;
}

}  // namespace ChapterRestart
