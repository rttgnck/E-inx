#pragma once

/**
 * @file CrossPlayWifi.h
 * @brief The WiFi picker, as CrossPlay's networked apps expect to open it.
 *
 * Connections, Hacker News and xkcd all open the firmware's WiFi selection
 * screen the same way:
 *
 *     startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
 *                            [this](const ActivityResult& r) { onWifiChosen(!r.isCancelled); });
 *
 * E-inx has that screen — `src/activity/network/WifiSelectionActivity.h` — but it
 * reports through an `onComplete(bool connected)` callback instead of popping
 * itself off a stack, because E-inx has no stack. This adapter is the join: it
 * constructs E-inx's screen with a completion callback that finishes the
 * sub-activity on the parent, turning "connected" into "not cancelled".
 *
 * The name is deliberate. Ported sources say `WifiSelectionActivity` and keep
 * saying it; only the include path changed.
 */

#include <functional>
#include <memory>

#include "activity/network/WifiSelectionActivity.h"
#include "CrossPlayCompat.h"

namespace crossplay {

/**
 * @brief Opens the WiFi picker over `parent` and reports the outcome.
 *
 * `onChosen(true)` means the device is on a network; `onChosen(false)` means the
 * player backed out. That is the same two-way answer upstream's `isCancelled`
 * carries, so the app's own handler is unchanged.
 */
inline void chooseWifi(CrossPlayActivity& parent, GfxRenderer& renderer, MappedInputManager& mappedInput,
                       const std::function<void(bool connected)>& onChosen) {
  auto screen = std::make_unique<WifiSelectionActivity>(renderer, mappedInput, [&parent, onChosen](
                                                                                   const bool connected) {
    ActivityResult result;
    result.isCancelled = !connected;
    // Finishing delivers the result to the handler the app registered, then
    // repaints the app underneath — which has been off-screen and holds a stale
    // framebuffer.
    parent.finishSubActivity(result);
    if (onChosen) onChosen(connected);
  });
  parent.startActivityForResult(std::move(screen), nullptr);
}

}  // namespace crossplay
