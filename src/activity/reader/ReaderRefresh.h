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
                             const uint8_t configuredMode, const bool reinforcementEligible = true) {
  HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH;
  const bool forced = forcedMode(configuredMode, mode);
  const bool forcedStrong = forced && mode != HalDisplay::FAST_REFRESH;
  if (forcedStrong) {
    if (Serial && renderer.reinforcementEnabled(GfxRenderer::ReinforcementTarget::ReaderBw)) {
      Serial.printf("[%lu] [GFX] X3 reader reinforcement bypass: forced mode=%u\n", millis(),
                    static_cast<unsigned>(configuredMode));
    }
    renderer.displayBuffer(mode);
    pagesUntilFullRefresh = cadence;
    return;
  }

  if (!reinforcementEligible && renderer.reinforcementEnabled(GfxRenderer::ReinforcementTarget::ReaderBw) && Serial) {
    Serial.printf("[%lu] [GFX] X3 reader reinforcement bypass: image/grayscale page\n", millis());
  }

  if (forced) {
    renderer.displayBuffer(mode);
    pagesUntilFullRefresh = cadence;
    return;
  }

  // The countdown drives maintenance whether or not reinforcement is on. It previously did not:
  // the reinforcement branch returned before ever reaching here, so the cadence the reader had
  // configured was decremented, reset, and never acted on, and the only thing that ever cleaned
  // up was a separate counter inside GfxRenderer.
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayMaintenance(reinforcementEligible);
    pagesUntilFullRefresh = cadence;
    return;
  }

  if (reinforcementEligible && renderer.reinforcementEnabled(GfxRenderer::ReinforcementTarget::ReaderBw)) {
    renderer.displayWithReinforcement(GfxRenderer::ReinforcementTarget::ReaderBw);
  } else {
    renderer.displayBuffer();
  }
  pagesUntilFullRefresh--;
}

}  // namespace ReaderRefresh
