/**
 * @file BookCovers.cpp
 * @brief Definitions for BookCovers.
 */

#include "BookCovers.h"

#include <Arduino.h>
#include <Epub.h>
#include <SDCardManager.h>
#include <Xtc.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "activity/settings/LibraryIndexer.h"
#include "util/SdIoMutex.h"
#include "util/StringUtils.h"

namespace {

bool isEpub(const std::string& path) { return StringUtils::checkFileExtension(path, ".epub"); }

bool isXtc(const std::string& path) {
  return StringUtils::checkFileExtension(path, ".xtc") || StringUtils::checkFileExtension(path, ".xtch");
}

void removeIfPresent(const std::string& path) {
  if (SdMan.exists(path.c_str())) SdMan.remove(path.c_str());
}

bool regenerateNow(const std::string& path) {
  SdIoMutex::Lock lock;
  if (isEpub(path)) {
    Epub epub(path, "/.metadata/epub");
    if (!epub.load()) return false;
    removeIfPresent(epub.getThumbBmpPath());
    removeIfPresent(epub.getThumbJpegPath());
    removeIfPresent(epub.getSmallThumbBmpPath());
    removeIfPresent(epub.getCoverBmpPath(false));
    removeIfPresent(epub.getCoverBmpPath(true));
    removeIfPresent(epub.getCoverJpegPath(false));
    removeIfPresent(epub.getCoverJpegPath(true));
    return epub.generateThumbBmp();
  }
  if (isXtc(path)) {
    Xtc xtc(path, "/.metadata/xtc");
    if (!xtc.load()) return false;
    removeIfPresent(xtc.getThumbBmpPath());
    removeIfPresent(xtc.getCoverBmpPath());
    return xtc.generateThumbBmp();
  }
  return false;
}

struct Job {
  std::string path;
  bool ok = false;
  volatile bool done = false;
};

}  // namespace

namespace BookCovers {

bool supports(const std::string& bookPath) { return isEpub(bookPath) || isXtc(bookPath); }

bool regenerate(const std::string& bookPath) {
  if (!supports(bookPath)) return false;

  Job job;
  job.path = bookPath;

  const BaseType_t created = xTaskCreate(
      [](void* parameter) {
        auto* job = static_cast<Job*>(parameter);
        job->ok = regenerateNow(job->path);
        job->done = true;
        vTaskDelete(nullptr);
      },
      "ThumbRegen", LibraryIndexer::kTaskStackBytes, &job, 1, nullptr);
  if (created != pdPASS) {
    Serial.printf("[%lu] [COVERS] No memory for the thumbnail task (free %u)\n", millis(),
                  static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }

  while (!job.done) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  Serial.printf("[%lu] [COVERS] %s thumbnail for %s\n", millis(), job.ok ? "Regenerated" : "Could not regenerate",
                bookPath.c_str());
  return job.ok;
}

}  // namespace BookCovers
