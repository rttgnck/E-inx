package com.einx.send

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.util.zip.CRC32

class BookFilesTest {

  @Test
  fun `checksum matches a known CRC32 and counts every byte`() {
    val payload = "The quick brown fox jumps over the lazy dog".toByteArray()
    val expected = CRC32().apply { update(payload) }.value

    val (crc, total) = BookFiles.checksum(ByteArrayInputStream(payload))

    assertEquals(expected, crc)
    assertEquals(payload.size.toLong(), total)
  }

  @Test
  fun `checksum streams across buffer boundaries`() {
    // Larger than the 64 KB read buffer, so the CRC has to be carried between reads.
    val payload = ByteArray(200_000) { (it % 251).toByte() }
    val expected = CRC32().apply { update(payload) }.value

    val (crc, total) = BookFiles.checksum(ByteArrayInputStream(payload))

    assertEquals(expected, crc)
    assertEquals(200_000L, total)
  }

  @Test
  fun `empty input still produces the canonical empty CRC`() {
    val (crc, total) = BookFiles.checksum(ByteArrayInputStream(ByteArray(0)))
    assertEquals(0L, crc)
    assertEquals(0L, total)
  }

  @Test
  fun `path separators never survive into the name sent to the reader`() {
    assertEquals("Dune.epub", BookFiles.sanitizeName("/storage/emulated/0/Books/Dune.epub"))
    assertEquals("Dune.epub", BookFiles.sanitizeName("..\\..\\Dune.epub"))
    assertFalse(BookFiles.sanitizeName("../../etc/passwd.txt").contains('/'))
  }

  @Test
  fun `leading dots are stripped so a book cannot masquerade as a part file`() {
    assertEquals("hidden.epub", BookFiles.sanitizeName(".hidden.epub"))
    assertTrue(BookFiles.sanitizeName(".Dune.epub.part").startsWith("Dune"))
  }

  @Test
  fun `names are clamped to what the reader will accept`() {
    val long = "a".repeat(300) + ".epub"
    assertEquals(96, BookFiles.sanitizeName(long).length)
  }

  @Test
  fun `characters FAT cannot store are replaced rather than dropped`() {
    assertEquals("a_b_c.epub", BookFiles.sanitizeName("a:b?c.epub"))
  }
}
