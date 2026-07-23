#pragma once

/**
 * @file IfFoundActivity.h
 * @brief Displays owner/contact instructions from /if_found.txt on the SD card.
 */

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "activity/Activity.h"

class IfFoundActivity final : public Activity {
 public:
  IfFoundActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::function<void()> onBack)
      : Activity("IfFound", renderer, mappedInput), onBack(std::move(onBack)) {}

  void onEnter() override;
  void loop() override;

 private:
  std::function<void()> onBack;
  std::vector<std::string> lines;
  int scrollOffset = 0;

  void loadText();
  void render();
};

