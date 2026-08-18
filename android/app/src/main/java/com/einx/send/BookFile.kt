package com.einx.send

import android.content.ContentResolver
import android.net.Uri
import android.provider.OpenableColumns
import java.io.InputStream
import java.util.zip.CRC32

/**
 * A book the user picked or shared, addressed by content Uri.
 *
 * Nothing here ever holds the book in memory: the size and CRC come from one streaming pass,
 * and the send does a second one. The Uri is all the access this app has or wants — no
 * storage permission is requested anywhere.
 */
data class BookFile(
  val uri: Uri,
  val name: String,
  val size: Long,
  /** Filled in by the streaming pass just before START; null until then. */
  val crc32: Long? = null,
  /** Overrides the user typed. Blank means send nothing and keep the book's own. */
  val title: String = "",
  val author: String = "",
) {
  val isSupported: Boolean get() = BleProtocol.isSupportedFilename(name)

  val extension: String get() = name.substringAfterLast('.', "").lowercase()
}

object BookFiles {

  private const val READ_BUFFER = 64 * 1024

  /**
   * Reads the display name and size out of the content provider.
   *
   * @return null when the Uri cannot be resolved — a share from an app that revoked the grant.
   */
  fun resolve(resolver: ContentResolver, uri: Uri): BookFile? {
    val projection = arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)

    resolver.query(uri, projection, null, null, null)?.use { cursor ->
      if (cursor.moveToFirst()) {
        val nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
        val sizeIndex = cursor.getColumnIndex(OpenableColumns.SIZE)

        val name = if (nameIndex >= 0 && !cursor.isNull(nameIndex)) {
          cursor.getString(nameIndex)
        } else {
          uri.lastPathSegment?.substringAfterLast('/')
        } ?: return null

        val size = if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) cursor.getLong(sizeIndex) else -1L
        val resolvedSize = if (size >= 0) size else measure(resolver, uri) ?: return null

        return BookFile(uri, sanitizeName(name), resolvedSize)
      }
    }

    // Some providers (notably plain file:// shares) have no cursor at all.
    val fallbackName = uri.lastPathSegment?.substringAfterLast('/') ?: return null
    val size = measure(resolver, uri) ?: return null
    return BookFile(uri, sanitizeName(fallbackName), size)
  }

  /**
   * Strips anything the reader would refuse, so a phone-side name never turns into a
   * path on the SD card. Directory separators are the ones that matter; the rest are
   * characters FAT will not store.
   */
  fun sanitizeName(raw: String): String {
    val base = raw.substringAfterLast('/').substringAfterLast('\\')
    val cleaned = base.map { c ->
      when {
        c.code < 0x20 || c.code == 0x7F -> '_'
        c in "\\/:*?\"<>|" -> '_'
        else -> c
      }
    }.joinToString("").trim().trimStart('.')

    return if (cleaned.isEmpty()) "book.epub" else cleaned.take(96)
  }

  /** Counts the bytes when the provider will not say how many there are. */
  private fun measure(resolver: ContentResolver, uri: Uri): Long? {
    return try {
      resolver.openInputStream(uri)?.use { stream ->
        var total = 0L
        val buffer = ByteArray(READ_BUFFER)
        while (true) {
          val read = stream.read(buffer)
          if (read <= 0) break
          total += read
        }
        total
      }
    } catch (e: Exception) {
      null
    }
  }

  /**
   * CRC32 of the whole file, computed by streaming it.
   *
   * @return the checksum and the number of bytes it covered — the caller compares that
   *         against the provider's reported size rather than trusting either alone.
   */
  fun checksum(stream: InputStream, onProgress: ((Long) -> Unit)? = null): Pair<Long, Long> {
    val crc = CRC32()
    val buffer = ByteArray(READ_BUFFER)
    var total = 0L

    while (true) {
      val read = stream.read(buffer)
      if (read <= 0) break
      crc.update(buffer, 0, read)
      total += read
      onProgress?.invoke(total)
    }

    return crc.value to total
  }
}
