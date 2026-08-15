#pragma once

/**
 * @file BluetoothTransferServer.h
 * @brief Public interface and types for BluetoothTransferServer.
 *
 * Receives a book over Bluetooth LE and streams it straight to the SD card. See
 * docs/BLE_FILE_TRANSFER.md for the wire protocol this implements.
 *
 * Threading: the GATT callbacks run on the NimBLE host task and only ever touch RAM —
 * incoming bytes land in a ring buffer. Every SD access happens in poll(), which the
 * owning activity calls from the main task. The SD card and the e-ink panel share one
 * SPI bus, so moving the writes onto the task that also paints the screen is what keeps
 * the two off the bus at the same time.
 */

#include <cstddef>
#include <cstdint>
#include <string>

/** Lifecycle of a single incoming book. */
enum class BleTransferState : uint8_t {
  Idle,      /**< Advertising, nothing in flight */
  Receiving, /**< START accepted, .part file open */
  Complete,  /**< Verified and renamed into place */
  Failed     /**< Rejected or aborted; .part already removed */
};

/** Everything the receive screen needs to paint itself, copied out under a lock. */
struct BleTransferStatus {
  BleTransferState state = BleTransferState::Idle;
  bool advertising = false;
  bool connected = false;
  std::string filename;          /**< Book currently arriving */
  uint32_t received = 0;         /**< Bytes committed to SD so far */
  uint32_t total = 0;            /**< Bytes promised by START */
  std::string errorCode;         /**< ERR_* code of the last failure, empty if none */
  std::string errorMessage;      /**< Human-readable form of the same failure */
  std::string lastCompletedName; /**< Book most recently accepted */
  unsigned long lastCompletedAt = 0;
  uint32_t lastCompletedBytes = 0;
  unsigned long lastCompletedMs = 0; /**< Wall time the accepted transfer took, for the speed readout */
};

class BluetoothTransferServer {
 public:
  BluetoothTransferServer() = default;
  ~BluetoothTransferServer();

  BluetoothTransferServer(const BluetoothTransferServer&) = delete;
  BluetoothTransferServer& operator=(const BluetoothTransferServer&) = delete;

  /** The advertised device name, e.g. "E-inx X3 A31F". Valid once begin() has succeeded. */
  static std::string advertisedName();

  /**
   * Brings up the BLE stack and starts advertising the transfer service.
   * @return false if the controller could not be started; nothing is left running in that case.
   */
  bool begin();

  /**
   * Tears the BLE stack all the way down, cancelling any transfer in flight and deleting its
   * .part file. Safe to call when begin() was never called or already failed.
   */
  void end();

  /**
   * Moves buffered bytes onto the SD card and finishes transfers whose FINISH has arrived.
   * Must be called from the main task, often — this is the only place the card is touched.
   */
  void poll();

  /** Snapshot of the current state, safe to call from the main task. */
  BleTransferStatus status() const;

  /** True while a phone is connected, whether or not it is sending. */
  bool isConnected() const;

  /** Cancels an in-flight transfer from the reader's side and removes the .part file. */
  void cancelTransfer();

  /** Whether this build can do BLE at all (false in the simulator). */
  static bool isSupported();

 private:
  bool running_ = false;
};
