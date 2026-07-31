/**
 * @file SleepCoverRepairActivity.cpp
 * @brief Definitions for SleepCoverRepairActivity.
 */

#include "SleepCoverRepairActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include "activity/system/SleepActivity.h"
#include "state/RecentBooks.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"

void SleepCoverRepairActivity::onEnter() {
  Activity::onEnter();

  renderer.clearScreen();
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, renderer.getScreenHeight() / 2 - 18,
                         "Regenerating sleep cover...", true);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  const bool ok = SleepActivity::regenerateLastReadCoverForSleep(&renderer);

  renderer.clearScreen();
  const char* message = ok ? "Sleep cover regenerated" : "No sleep cover to regenerate";
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, renderer.getScreenHeight() / 2 - 18, message, true);
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, "Back", "Done", "", "");
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepCoverRepairActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (onDone) {
      onDone();
    }
  }
}
