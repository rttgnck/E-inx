#include "system/SleepWakeTraceStore.h"

#ifndef SIMULATOR

#include <SDCardManager.h>

namespace {
constexpr uint32_t TRACE_FILE_MAGIC = 0x49585452UL;
constexpr uint8_t TRACE_FILE_VERSION = 1;
constexpr char TRACE_FILE[] = "/.system/sleep_wake_trace.bin";
constexpr char TRACE_TEMP_FILE[] = "/.system/sleep_wake_trace.tmp";
constexpr char TRACE_BACKUP_FILE[] = "/.system/sleep_wake_trace.bak";

template <typename T>
bool writeValue(FsFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(value)) == sizeof(value);
}

template <typename T>
bool readValue(FsFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(value)) == sizeof(value);
}

bool hasRetainedSleepCycle(const HalGPIO::SleepWakeTraceSnapshot& snapshot) {
  for (uint8_t i = 0; i < snapshot.count; ++i) {
    if (snapshot.entries[i].event == HalGPIO::SleepWakeTraceEvent::SleepPlan) {
      return true;
    }
  }
  return false;
}

bool writeSnapshot(FsFile& file, const HalGPIO::SleepWakeTraceSnapshot& snapshot) {
  const uint16_t reserved = 0;
  if (!writeValue(file, TRACE_FILE_MAGIC) || !writeValue(file, TRACE_FILE_VERSION) ||
      !writeValue(file, snapshot.count) || !writeValue(file, reserved)) {
    return false;
  }

  for (uint8_t i = 0; i < snapshot.count; ++i) {
    const HalGPIO::SleepWakeTraceEntry& entry = snapshot.entries[i];
    const uint8_t event = static_cast<uint8_t>(entry.event);
    if (!writeValue(file, entry.sequence) || !writeValue(file, entry.rtcTickLow) || !writeValue(file, entry.arg0) ||
        !writeValue(file, entry.arg1) || !writeValue(file, event)) {
      return false;
    }
  }
  return file.sync();
}

bool readSnapshot(const char* path, HalGPIO::SleepWakeTraceSnapshot& snapshot) {
  FsFile file;
  if (!SdMan.openFileForRead("SWT", path, file)) {
    return false;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint8_t count = 0;
  uint16_t reserved = 0;
  const bool headerValid = readValue(file, magic) && readValue(file, version) && readValue(file, count) &&
                           readValue(file, reserved) && magic == TRACE_FILE_MAGIC && version == TRACE_FILE_VERSION &&
                           count <= HalGPIO::SLEEP_WAKE_TRACE_CAPACITY;
  if (!headerValid) {
    file.close();
    return false;
  }

  snapshot.count = count;
  for (uint8_t i = 0; i < count; ++i) {
    HalGPIO::SleepWakeTraceEntry& entry = snapshot.entries[i];
    uint8_t event = 0;
    if (!readValue(file, entry.sequence) || !readValue(file, entry.rtcTickLow) || !readValue(file, entry.arg0) ||
        !readValue(file, entry.arg1) || !readValue(file, event) ||
        event < static_cast<uint8_t>(HalGPIO::SleepWakeTraceEvent::SleepPlan) ||
        event > static_cast<uint8_t>(HalGPIO::SleepWakeTraceEvent::PowerGesture)) {
      snapshot.count = 0;
      file.close();
      return false;
    }
    entry.event = static_cast<HalGPIO::SleepWakeTraceEvent>(event);
  }

  file.close();
  return true;
}
}  // namespace

bool saveSleepWakeTraceCheckpoint(const bool requireRetainedSleepCycle) {
  const HalGPIO::SleepWakeTraceSnapshot snapshot = HalGPIO::getSleepWakeTrace();
  if (snapshot.count == 0 || (requireRetainedSleepCycle && !hasRetainedSleepCycle(snapshot))) {
    return false;
  }

  SdMan.mkdir("/.system");
  SdMan.remove(TRACE_TEMP_FILE);

  FsFile file;
  if (!SdMan.openFileForWrite("SWT", TRACE_TEMP_FILE, file)) {
    return false;
  }
  const bool writeOk = writeSnapshot(file, snapshot);
  file.close();
  if (!writeOk) {
    SdMan.remove(TRACE_TEMP_FILE);
    return false;
  }

  SdMan.remove(TRACE_BACKUP_FILE);
  const bool hadPrevious = SdMan.exists(TRACE_FILE);
  if (hadPrevious && !SdMan.rename(TRACE_FILE, TRACE_BACKUP_FILE)) {
    SdMan.remove(TRACE_TEMP_FILE);
    return false;
  }
  if (!SdMan.rename(TRACE_TEMP_FILE, TRACE_FILE)) {
    if (hadPrevious) {
      SdMan.rename(TRACE_BACKUP_FILE, TRACE_FILE);
    }
    SdMan.remove(TRACE_TEMP_FILE);
    return false;
  }
  SdMan.remove(TRACE_BACKUP_FILE);
  return true;
}

HalGPIO::SleepWakeTraceSnapshot loadSleepWakeTraceCheckpoint() {
  HalGPIO::SleepWakeTraceSnapshot snapshot;
  if (readSnapshot(TRACE_FILE, snapshot)) {
    return snapshot;
  }
  readSnapshot(TRACE_BACKUP_FILE, snapshot);
  return snapshot;
}

void clearSleepWakeTraceCheckpoint() {
  SdMan.remove(TRACE_FILE);
  SdMan.remove(TRACE_TEMP_FILE);
  SdMan.remove(TRACE_BACKUP_FILE);
}

#endif
