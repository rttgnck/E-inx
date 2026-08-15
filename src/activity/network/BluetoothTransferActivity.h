#pragma once

/**
 * @file BluetoothTransferActivity.h
 * @brief Public interface and types for BluetoothTransferActivity.
 */

#include <functional>
#include <string>

#include "activity/ActivityWithSubactivity.h"
#include "activity/Menu.h"
#include "network/BluetoothTransferServer.h"

/**
 * @brief Receive screen for Bluetooth book transfer.
 *
 * The radio's lifetime is exactly this screen's lifetime: BLE comes up in onEnter() and is
 * shut all the way down in onExit(), so nothing is advertising once the user has left.
 */
class BluetoothTransferActivity final : public ActivityWithSubactivity, public Menu {
 public:
  BluetoothTransferActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::function<void()>& onComplete)
      : ActivityWithSubactivity("BluetoothTransfer", renderer, mappedInput), Menu(), onComplete(onComplete) {
    tabSelectorIndex = 4;
  }

  ~BluetoothTransferActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;

  /** The radio needs servicing far more often than the idle tick allows. */
  bool skipLoopDelay() override { return true; }

  /** Sleeping mid-transfer would strand a .part file, and the phone is waiting on us. */
  bool preventAutoSleep() override { return true; }

 private:
  void render() const;
  void navigateToSelectedMenu() override {}

  /** True when the snapshot differs from what is currently on the panel by enough to redraw. */
  bool needsRedraw(const BleTransferStatus& next) const;

  BluetoothTransferServer server;
  BleTransferStatus painted;      /**< What the panel is currently showing */
  unsigned long lastPaintAt = 0;  /**< Throttles progress redraws; e-ink is slow */
  bool startFailed = false;       /**< begin() refused, so the screen only offers a way out */
  bool exitRequested = false;

  const std::function<void()> onComplete;
};
