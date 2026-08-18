package com.einx.send

import org.json.JSONObject
import java.util.UUID

/**
 * The wire protocol shared with the firmware.
 *
 * This file and src/network/BluetoothTransferServer.cpp are two halves of one contract;
 * docs/BLE_FILE_TRANSFER.md is the description of it. Change all three together.
 */
object BleProtocol {

  const val VERSION = 1

  val SERVICE_UUID: UUID = UUID.fromString("e1780001-5b41-4d2e-9a63-7c8f1b0d4e21")
  val CONTROL_UUID: UUID = UUID.fromString("e1780002-5b41-4d2e-9a63-7c8f1b0d4e21")
  val DATA_UUID: UUID = UUID.fromString("e1780003-5b41-4d2e-9a63-7c8f1b0d4e21")
  val STATUS_UUID: UUID = UUID.fromString("e1780004-5b41-4d2e-9a63-7c8f1b0d4e21")

  /** The descriptor every BLE stack uses to subscribe to notifications. */
  val CCC_DESCRIPTOR_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

  /** Extensions the reader will accept. Checked here too so a bad pick fails before connecting. */
  val SUPPORTED_EXTENSIONS = listOf("epub", "txt", "xtc", "xtch")

  /**
   * START. The reader replies with [Status.Receiving] once the .part file is open, or with
   * [Status.Error] if it will not take the book — nothing is sent until one of those arrives.
   */
  fun startCommand(
    name: String,
    size: Long,
    crc32: Long,
    title: String = "",
    author: String = "",
  ): ByteArray =
    JSONObject()
      .put("cmd", "start")
      .put("protocol", VERSION)
      .put("name", name)
      .put("size", size)
      // Hex text, so neither side has to agree on how an unsigned 32-bit number is spelled.
      .put("crc32", String.format("%08x", crc32 and 0xFFFFFFFFL))
      .apply {
        // Additive to protocol 1, and omitted when blank: a reader that predates these ignores
        // unknown fields, and a blank one means "keep whatever the book says about itself".
        if (title.isNotBlank()) put("title", title.trim())
        if (author.isNotBlank()) put("author", author.trim())
      }
      .toString()
      .toByteArray(Charsets.UTF_8)

  fun finishCommand(): ByteArray =
    JSONObject().put("cmd", "finish").toString().toByteArray(Charsets.UTF_8)

  fun abortCommand(): ByteArray =
    JSONObject().put("cmd", "abort").toString().toByteArray(Charsets.UTF_8)

  /** A STATUS notification from the reader. */
  sealed interface Status {
    data class Ready(val protocol: Int, val readerName: String, val maxSize: Long) : Status
    data class Receiving(val name: String, val total: Long) : Status
    data class Progress(val received: Long, val total: Long) : Status
    data class Complete(val name: String, val size: Long) : Status
    data class Error(val code: String, val message: String) : Status
    data class Unknown(val raw: String) : Status
  }

  fun parseStatus(payload: ByteArray): Status {
    val raw = String(payload, Charsets.UTF_8)
    val json = try {
      JSONObject(raw)
    } catch (e: Exception) {
      return Status.Unknown(raw)
    }

    return when (json.optString("st")) {
      "ready" -> Status.Ready(
        protocol = json.optInt("proto", 0),
        readerName = json.optString("name"),
        maxSize = json.optLong("maxSize", 0L),
      )

      "receiving" -> Status.Receiving(
        name = json.optString("name"),
        total = json.optLong("total", 0L),
      )

      "progress" -> Status.Progress(
        received = json.optLong("recv", 0L),
        total = json.optLong("total", 0L),
      )

      "complete" -> Status.Complete(
        name = json.optString("name"),
        size = json.optLong("size", 0L),
      )

      "error" -> Status.Error(
        code = json.optString("code", "ERR_UNKNOWN"),
        message = json.optString("msg", "The reader rejected the transfer"),
      )

      else -> Status.Unknown(raw)
    }
  }

  /** True when the reader will accept a book with this name. Mirrors the firmware's check. */
  fun isSupportedFilename(name: String): Boolean {
    val extension = name.substringAfterLast('.', "").lowercase()
    return extension in SUPPORTED_EXTENSIONS
  }
}
