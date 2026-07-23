#pragma once

#include <GfxRenderer.h>

#include "state/SystemSetting.h"

namespace ReaderRefresh {

inline bool forcedMode(const uint8_t configuredMode, HalDisplay::RefreshMode& mode) {
  switch (configuredMode) {
    case SystemSetting::READER_REFRESH_FAST:
      mode = HalDisplay::FAST_REFRESH;
      return true;
    case SystemSetting::READER_REFRESH_HALF:
      mode = HalDisplay::HALF_REFRESH;
      return true;
    case SystemSetting::READER_REFRESH_FULL:
      mode = HalDisplay::FULL_REFRESH;
      return true;
    case SystemSetting::READER_REFRESH_AUTO:
    default:
      return false;
  }
}

inline void displayWithCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh, const uint8_t cadence,
                             const uint8_t configuredMode) {
  HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH;
  if (forcedMode(configuredMode, mode)) {
    renderer.displayBuffer(mode);
    pagesUntilFullRefresh = cadence;
    return;
  }

  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = cadence;
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

}  // namespace ReaderRefresh
