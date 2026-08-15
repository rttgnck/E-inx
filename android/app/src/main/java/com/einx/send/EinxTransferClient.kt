package com.einx.send

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.content.ContentResolver
import android.content.Context
import android.os.Build
import android.util.Log
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.withTimeout
import java.io.InputStream
import java.util.concurrent.atomic.AtomicBoolean

/** How a transfer ended. */
sealed interface TransferOutcome {
  data object Success : TransferOutcome
  data class Failure(val code: String, val message: String) : TransferOutcome
}

/**
 * Sends one book to one reader over BLE.
 *
 * The file is streamed: a chunk is read from the content Uri, written to the DATA
 * characteristic, and only once the stack has acknowledged that write is the next chunk read.
 * That acknowledgement is the flow control — the reader is putting each chunk on an SD card,
 * which is slower than the radio, and outrunning it is what would corrupt a book.
 */
class EinxTransferClient(private val context: Context) {

  internal companion object {
    const val TAG = "EinxTransferClient"

    const val CONNECT_TIMEOUT_MS = 20_000L
    const val SETUP_TIMEOUT_MS = 20_000L
    const val WRITE_TIMEOUT_MS = 15_000L
    const val START_ACK_TIMEOUT_MS = 20_000L

    // The reader verifies size and CRC and renames the file before answering FINISH.
    const val FINISH_TIMEOUT_MS = 90_000L

    /** ATT overhead: three bytes of the MTU are the opcode and handle. */
    const val ATT_HEADER_BYTES = 3

    /**
     * A GATT attribute value tops out at 512 bytes regardless of the MTU, and Android
     * throws rather than truncating. MTU 517 would otherwise give a 514-byte chunk.
     */
    const val MAX_ATTRIBUTE_BYTES = 512

    /** Used until the reader and phone have agreed on something larger. */
    const val DEFAULT_CHUNK_BYTES = 20
  }

  private val cancelRequested = AtomicBoolean(false)

  /** Asks the in-flight transfer to stop. The reader is told, so it deletes its .part file. */
  fun requestCancel() {
    cancelRequested.set(true)
  }

  @SuppressLint("MissingPermission")
  suspend fun send(
    device: BluetoothDevice,
    book: BookFile,
    resolver: ContentResolver,
    onProgress: (sent: Long, total: Long) -> Unit,
  ): TransferOutcome {
    cancelRequested.set(false)

    val connectionState = Channel<Int>(Channel.CONFLATED)
    val servicesDiscovered = Channel<Int>(Channel.CONFLATED)
    val mtuChanged = Channel<Int>(Channel.CONFLATED)
    val descriptorWritten = Channel<Int>(Channel.CONFLATED)
    val statusMessages = Channel<BleProtocol.Status>(Channel.UNLIMITED)

    // Replaced immediately before each write, then awaited. Only one write is ever in flight.
    var writeAck: CompletableDeferred<Int>? = null

    val callback = object : BluetoothGattCallback() {
      override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
        connectionState.trySend(newState)
        if (newState == BluetoothProfile.STATE_DISCONNECTED) {
          // Unblocks anything waiting rather than letting it sit until its timeout.
          writeAck?.complete(BluetoothGatt.GATT_FAILURE)
          statusMessages.trySend(
            BleProtocol.Status.Error("ERR_DISCONNECTED", "The reader disconnected")
          )
        }
      }

      override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
        servicesDiscovered.trySend(status)
      }

      override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
        mtuChanged.trySend(if (status == BluetoothGatt.GATT_SUCCESS) mtu else -1)
      }

      override fun onDescriptorWrite(
        gatt: BluetoothGatt,
        descriptor: BluetoothGattDescriptor,
        status: Int,
      ) {
        descriptorWritten.trySend(status)
      }

      override fun onCharacteristicWrite(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        status: Int,
      ) {
        writeAck?.complete(status)
      }

      // API 33+ delivers the value with the callback.
      override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
        value: ByteArray,
      ) {
        statusMessages.trySend(BleProtocol.parseStatus(value))
      }

      @Deprecated("Superseded by the overload carrying the value on API 33+")
      @Suppress("DEPRECATION")
      override fun onCharacteristicChanged(
        gatt: BluetoothGatt,
        characteristic: BluetoothGattCharacteristic,
      ) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
          characteristic.value?.let { statusMessages.trySend(BleProtocol.parseStatus(it)) }
        }
      }
    }

    var gatt: BluetoothGatt? = null
    var stream: InputStream? = null

    try {
      gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
        ?: return TransferOutcome.Failure("ERR_CONNECT", "Could not open a connection")

      withTimeout(CONNECT_TIMEOUT_MS) {
        while (connectionState.receive() != BluetoothProfile.STATE_CONNECTED) {
          // Keep waiting; a disconnect here means the attempt failed and the timeout applies.
        }
      }

      // Ask for the fastest connection interval the phone will grant. This is the single
      // biggest factor in transfer speed — it decides how often data can be sent at all.
      gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)

      var chunkSize = DEFAULT_CHUNK_BYTES
      gatt.requestMtu(517)
      val negotiatedMtu = withTimeout(SETUP_TIMEOUT_MS) { mtuChanged.receive() }
      if (negotiatedMtu > 0) {
        chunkSize = chunkSizeForMtu(negotiatedMtu)
      }
      Log.i(TAG, "Negotiated MTU $negotiatedMtu, sending $chunkSize bytes per write")

      if (!gatt.discoverServices()) {
        return TransferOutcome.Failure("ERR_CONNECT", "Could not inspect the reader")
      }
      val discoveryStatus = withTimeout(SETUP_TIMEOUT_MS) { servicesDiscovered.receive() }
      if (discoveryStatus != BluetoothGatt.GATT_SUCCESS) {
        return TransferOutcome.Failure("ERR_CONNECT", "Could not inspect the reader")
      }

      val service = gatt.getService(BleProtocol.SERVICE_UUID)
        ?: return TransferOutcome.Failure(
          "ERR_SERVICE",
          "This device is not running Bluetooth Transfer",
        )

      val control = service.getCharacteristic(BleProtocol.CONTROL_UUID)
      val data = service.getCharacteristic(BleProtocol.DATA_UUID)
      val statusCharacteristic = service.getCharacteristic(BleProtocol.STATUS_UUID)
      if (control == null || data == null || statusCharacteristic == null) {
        return TransferOutcome.Failure("ERR_SERVICE", "The reader's transfer service is incomplete")
      }

      gatt.setCharacteristicNotification(statusCharacteristic, true)
      val ccc = statusCharacteristic.getDescriptor(BleProtocol.CCC_DESCRIPTOR_UUID)
        ?: return TransferOutcome.Failure("ERR_SERVICE", "The reader cannot report progress")
      writeDescriptor(gatt, ccc, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
      withTimeout(SETUP_TIMEOUT_MS) { descriptorWritten.receive() }

      // START, then wait for the reader to say the file is open before sending a byte.
      val startAck = CompletableDeferred<Int>()
      writeAck = startAck
      if (!writeCharacteristic(
          gatt,
          control,
          BleProtocol.startCommand(book.name, book.size, requireNotNull(book.crc32) {
            "The book must be checksummed before it is sent"
          }),
          BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
        )
      ) {
        return TransferOutcome.Failure("ERR_WRITE", "Could not start the transfer")
      }
      withTimeout(WRITE_TIMEOUT_MS) { startAck.await() }

      val startResponse = withTimeout(START_ACK_TIMEOUT_MS) { awaitDecision(statusMessages) }
      when (startResponse) {
        is BleProtocol.Status.Error ->
          return TransferOutcome.Failure(startResponse.code, startResponse.message)

        !is BleProtocol.Status.Receiving ->
          return TransferOutcome.Failure("ERR_PROTOCOL", "The reader did not accept the book")

        else -> Unit
      }

      // Stream the body.
      stream = resolver.openInputStream(book.uri)
        ?: return TransferOutcome.Failure("ERR_READ", "Could not read the book from this phone")

      val buffer = ByteArray(chunkSize)
      var sent = 0L

      while (sent < book.size) {
        if (cancelRequested.get()) {
          abort(gatt, control)
          return TransferOutcome.Failure("ERR_ABORTED", "Cancelled")
        }

        val read = stream.read(buffer)
        if (read <= 0) break

        val payload = if (read == buffer.size) buffer else buffer.copyOf(read)

        val chunkAck = CompletableDeferred<Int>()
        writeAck = chunkAck
        if (!writeCharacteristic(
            gatt,
            data,
            payload,
            BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE,
          )
        ) {
          abort(gatt, control)
          return TransferOutcome.Failure("ERR_WRITE", "The connection stopped accepting data")
        }

        val ackStatus = withTimeout(WRITE_TIMEOUT_MS) { chunkAck.await() }
        if (ackStatus != BluetoothGatt.GATT_SUCCESS) {
          return TransferOutcome.Failure("ERR_WRITE", "The connection dropped mid-transfer")
        }

        sent += read
        onProgress(sent, book.size)

        // An error raised by the reader mid-stream (a full card, say) ends things here
        // rather than after another two minutes of writing into a closed file.
        drainErrors(statusMessages)?.let { return it }
      }

      if (sent != book.size) {
        abort(gatt, control)
        return TransferOutcome.Failure("ERR_READ", "The book was shorter than this phone reported")
      }

      val finishAck = CompletableDeferred<Int>()
      writeAck = finishAck
      if (!writeCharacteristic(
          gatt,
          control,
          BleProtocol.finishCommand(),
          BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
        )
      ) {
        return TransferOutcome.Failure("ERR_WRITE", "Could not finish the transfer")
      }
      withTimeout(WRITE_TIMEOUT_MS) { finishAck.await() }

      return when (val finish = withTimeout(FINISH_TIMEOUT_MS) { awaitDecision(statusMessages) }) {
        is BleProtocol.Status.Complete -> TransferOutcome.Success
        is BleProtocol.Status.Error -> TransferOutcome.Failure(finish.code, finish.message)
        else -> TransferOutcome.Failure("ERR_PROTOCOL", "The reader did not confirm the book")
      }
    } catch (e: TimeoutCancellationException) {
      Log.w(TAG, "Timed out", e)
      return TransferOutcome.Failure("ERR_TIMEOUT", "The reader stopped responding")
    } catch (e: SecurityException) {
      Log.w(TAG, "Missing Bluetooth permission", e)
      return TransferOutcome.Failure("ERR_PERMISSION", "Bluetooth permission was denied")
    } catch (e: Exception) {
      Log.w(TAG, "Transfer failed", e)
      return TransferOutcome.Failure("ERR_UNKNOWN", e.message ?: "The transfer failed")
    } finally {
      try {
        stream?.close()
      } catch (ignored: Exception) {
      }
      try {
        gatt?.disconnect()
        gatt?.close()
      } catch (ignored: SecurityException) {
      }
      statusMessages.close()
    }
  }

  /** Waits past progress chatter for the next message that actually decides something. */
  private suspend fun awaitDecision(
    statusMessages: Channel<BleProtocol.Status>,
  ): BleProtocol.Status {
    while (true) {
      when (val message = statusMessages.receive()) {
        is BleProtocol.Status.Progress, is BleProtocol.Status.Ready, is BleProtocol.Status.Unknown ->
          Unit

        else -> return message
      }
    }
  }

  /** Non-blocking peek for an error the reader raised while we were streaming. */
  private fun drainErrors(statusMessages: Channel<BleProtocol.Status>): TransferOutcome.Failure? {
    while (true) {
      val message = statusMessages.tryReceive().getOrNull() ?: return null
      if (message is BleProtocol.Status.Error) {
        return TransferOutcome.Failure(message.code, message.message)
      }
    }
  }

  @SuppressLint("MissingPermission")
  private suspend fun abort(gatt: BluetoothGatt, control: BluetoothGattCharacteristic) {
    try {
      writeCharacteristic(
        gatt,
        control,
        BleProtocol.abortCommand(),
        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT,
      )
      // Give the write a moment to leave the phone before the connection is torn down.
      kotlinx.coroutines.delay(250)
    } catch (ignored: Exception) {
    }
  }

  @SuppressLint("MissingPermission")
  @Suppress("DEPRECATION")
  private fun writeCharacteristic(
    gatt: BluetoothGatt,
    characteristic: BluetoothGattCharacteristic,
    value: ByteArray,
    writeType: Int,
  ): Boolean {
    // Android throws rather than truncating an oversize attribute write, and the throw
    // surfaces as an unrecognisable error several layers up. Refuse it here instead.
    if (value.size > MAX_ATTRIBUTE_BYTES) {
      Log.e(TAG, "Refusing a ${value.size} byte write; the GATT limit is $MAX_ATTRIBUTE_BYTES")
      return false
    }

    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      gatt.writeCharacteristic(characteristic, value, writeType) == BluetoothStatusCodes.SUCCESS
    } else {
      characteristic.writeType = writeType
      characteristic.value = value
      gatt.writeCharacteristic(characteristic)
    }
  }

  @SuppressLint("MissingPermission")
  @Suppress("DEPRECATION")
  private fun writeDescriptor(
    gatt: BluetoothGatt,
    descriptor: BluetoothGattDescriptor,
    value: ByteArray,
  ): Boolean {
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
      gatt.writeDescriptor(descriptor, value) == BluetoothStatusCodes.SUCCESS
    } else {
      descriptor.value = value
      gatt.writeDescriptor(descriptor)
    }
  }
}

/**
 * Payload size for one DATA write, given the negotiated ATT MTU.
 *
 * Three bytes of the MTU are ATT overhead, and a GATT attribute value cannot exceed 512
 * bytes however large the MTU is — an MTU of 517 does not mean a 514-byte write is legal.
 */
internal fun chunkSizeForMtu(mtu: Int): Int =
  (mtu - EinxTransferClient.ATT_HEADER_BYTES)
    .coerceIn(EinxTransferClient.DEFAULT_CHUNK_BYTES, EinxTransferClient.MAX_ATTRIBUTE_BYTES)
