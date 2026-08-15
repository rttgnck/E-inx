package com.einx.send

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.ParcelUuid
import android.util.Log

/** A reader that is advertising the transfer service right now. */
data class DiscoveredReader(
  val device: BluetoothDevice,
  val name: String,
  val address: String,
  val rssi: Int,
)

/**
 * Finds nearby readers.
 *
 * The scan filters on the transfer service UUID, so the list only ever contains devices
 * that are sitting on the Bluetooth Transfer screen with the service running — no other
 * Bluetooth device the phone can see is shown, and nothing is learned about them.
 */
class ReaderScanner(context: Context) {

  private companion object {
    const val TAG = "ReaderScanner"
  }

  private val adapter: BluetoothAdapter? =
    (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

  private var callback: ScanCallback? = null

  val isBluetoothOn: Boolean get() = adapter?.isEnabled == true

  @SuppressLint("MissingPermission")
  fun start(onFound: (DiscoveredReader) -> Unit, onFailed: (String) -> Unit) {
    val scanner = adapter?.bluetoothLeScanner
    if (adapter == null || !adapter.isEnabled || scanner == null) {
      onFailed("Turn Bluetooth on to find your reader")
      return
    }

    stop()

    val filters = listOf(
      ScanFilter.Builder()
        .setServiceUuid(ParcelUuid(BleProtocol.SERVICE_UUID))
        .build()
    )

    val settings = ScanSettings.Builder()
      .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
      .build()

    val scanCallback = object : ScanCallback() {
      override fun onScanResult(callbackType: Int, result: ScanResult) {
        val device = result.device ?: return
        val name = try {
          result.scanRecord?.deviceName ?: device.name ?: "E-inx reader"
        } catch (e: SecurityException) {
          "E-inx reader"
        }
        onFound(DiscoveredReader(device, name, device.address, result.rssi))
      }

      override fun onScanFailed(errorCode: Int) {
        Log.w(TAG, "Scan failed with code $errorCode")
        onFailed("Could not search for readers (error $errorCode)")
      }
    }

    callback = scanCallback

    try {
      scanner.startScan(filters, settings, scanCallback)
    } catch (e: SecurityException) {
      callback = null
      onFailed("Bluetooth permission was denied")
    }
  }

  @SuppressLint("MissingPermission")
  fun stop() {
    val active = callback ?: return
    callback = null
    try {
      adapter?.bluetoothLeScanner?.stopScan(active)
    } catch (e: SecurityException) {
      // The permission was revoked while scanning; the scan is already gone with it.
    }
  }
}
