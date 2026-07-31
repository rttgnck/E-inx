/**
 * @file Session.cpp
 * @brief Definitions for Session.
 */

#include "state/Session.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <Serialization.h>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 5;
constexpr char STATE_FILE[] = "/.system/state.bin";
}  // namespace

Session Session::instance;

bool Session::saveToFile() const {
  std::string dirPath = STATE_FILE;
  size_t lastSlash = dirPath.find_last_of('/');
  if (lastSlash != std::string::npos) {
    dirPath.resize(lastSlash);
  }

  FsFile outputFile;
  if (!SdMan.openFileForWrite("CPS", STATE_FILE, outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, STATE_FILE_VERSION);
  serialization::writeString(outputFile, lastRead);
  serialization::writePod(outputFile, lastSleepImage);
  serialization::writePod(outputFile, sleepImageShuffleSeed);
  serialization::writeString(outputFile, lastSleepImagePath);
  serialization::writePod(outputFile, lastSleepTimerArmSeconds);
  serialization::writePod(outputFile, sleepTimerArmCount);
  serialization::writePod(outputFile, sleepTimerWakeCount);
  serialization::writePod(outputFile, lastWakeReason);

  outputFile.close();
  return true;
}

bool Session::loadFromFile() {
  if (!SdMan.exists(STATE_FILE)) {
    lastRead = "";
    lastSleepImagePath = "";
    lastSleepImage = 0;
    sleepImageShuffleSeed = 0;
    lastSleepTimerArmSeconds = 0;
    sleepTimerArmCount = 0;
    sleepTimerWakeCount = 0;
    lastWakeReason = 0;
    return saveToFile();
  }

  FsFile inputFile;
  if (!SdMan.openFileForRead("CPS", STATE_FILE, inputFile)) {
    lastRead = "";
    lastSleepImagePath = "";
    lastSleepImage = 0;
    sleepImageShuffleSeed = 0;
    lastSleepTimerArmSeconds = 0;
    sleepTimerArmCount = 0;
    sleepTimerWakeCount = 0;
    lastWakeReason = 0;
    return saveToFile();
  }

  uint8_t version;
  serialization::readPod(inputFile, version);

  if (version > STATE_FILE_VERSION) {
    inputFile.close();
    lastRead = "";
    lastSleepImagePath = "";
    lastSleepImage = 0;
    sleepImageShuffleSeed = 0;
    lastSleepTimerArmSeconds = 0;
    sleepTimerArmCount = 0;
    sleepTimerWakeCount = 0;
    lastWakeReason = 0;
    return saveToFile();
  }

  serialization::readString(inputFile, lastRead);

  if (version >= 3) {
    serialization::readPod(inputFile, lastSleepImage);
    serialization::readPod(inputFile, sleepImageShuffleSeed);
  } else if (version >= 2) {
    uint8_t legacyLastSleepImage = 0;
    serialization::readPod(inputFile, legacyLastSleepImage);
    lastSleepImage = legacyLastSleepImage;
    sleepImageShuffleSeed = 0;
  } else {
    lastSleepImage = 0;
    sleepImageShuffleSeed = 0;
  }

  if (version >= 4) {
    serialization::readString(inputFile, lastSleepImagePath);
    serialization::readPod(inputFile, lastSleepTimerArmSeconds);
    if (version >= 5) {
      serialization::readPod(inputFile, sleepTimerArmCount);
    } else {
      sleepTimerArmCount = 0;
    }
    serialization::readPod(inputFile, sleepTimerWakeCount);
    serialization::readPod(inputFile, lastWakeReason);
  } else {
    lastSleepImagePath = "";
    lastSleepTimerArmSeconds = 0;
    sleepTimerArmCount = 0;
    sleepTimerWakeCount = 0;
    lastWakeReason = 0;
  }

  inputFile.close();

  if (lastRead.length() > 512) {
    lastRead = "";
    lastSleepImagePath = "";
    lastSleepImage = 0;
    sleepImageShuffleSeed = 0;
    lastSleepTimerArmSeconds = 0;
    sleepTimerArmCount = 0;
    sleepTimerWakeCount = 0;
    lastWakeReason = 0;
    return saveToFile();
  }
  if (lastSleepImagePath.length() > 256) {
    lastSleepImagePath = "";
  }

  return true;
}
