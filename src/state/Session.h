#pragma once

/**
 * @file Session.h
 * @brief Public interface and types for Session.
 */

#include <iosfwd>
#include <string>

class Session {
  static Session instance;

 public:
  std::string lastRead;
  std::string lastSleepImagePath;
  uint32_t lastSleepImage;
  uint32_t sleepImageShuffleSeed;
  uint32_t lastSleepTimerArmSeconds;
  uint32_t sleepTimerArmCount;
  uint32_t sleepTimerWakeCount;
  uint8_t lastWakeReason;
  ~Session() = default;

  static Session& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();
};

#define APP_STATE Session::getInstance()
