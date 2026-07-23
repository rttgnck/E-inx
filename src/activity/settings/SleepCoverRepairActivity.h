#pragma once

/**
 * @file SleepCoverRepairActivity.h
 * @brief Settings action for regenerating the cached sleep cover.
 */

#include <functional>

#include "../Activity.h"

class SleepCoverRepairActivity final : public Activity {
 public:
  SleepCoverRepairActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onDone)
      : Activity("SleepCoverRepair", renderer, mappedInput), onDone(onDone) {}

  void onEnter() override;
  void loop() override;

 private:
  std::function<void()> onDone;
};
