package com.einx.send

import java.io.InputStream
import java.util.zip.ZipInputStream

/** Title and author as the EPUB describes itself. Either may be blank. */
data class EpubMetadata(val title: String, val author: String) {
  val isEmpty: Boolean get() = title.isBlank() && author.isBlank()

  companion object {
    val EMPTY = EpubMetadata("", "")
  }
}

/**
 * Reads the title and author out of an EPUB so the edit fields can start from what the book
 * actually says, rather than from a blank box.
 *
 * This only ever reads. Sending an edited title does not rewrite the file — the reader keeps a
 * per-book override, the same one its own Edit Metadata screen writes — so the EPUB that arrives
 * is byte-for-byte the one that was picked and its checksum still matches.
 */
object EpubMetadataReader {

  /** An OPF is a few KB; anything beyond this is not one and is not worth buffering. */
  private const val MAX_OPF_BYTES = 512 * 1024

  /**
   * Scans for the package document and parses it.
   *
   * The spec route is META-INF/container.xml pointing at the OPF, but a ZipInputStream reads
   * entries in whatever order they were written, so following that pointer can mean a second
   * pass. Every EPUB has exactly one .opf, so taking the first one found reads the file once.
   *
   * @return [EpubMetadata.EMPTY] when the file is not an EPUB, is damaged, or names neither.
   */
  fun read(stream: InputStream): EpubMetadata {
    return try {
      ZipInputStream(stream).use { zip ->
        while (true) {
          val entry = zip.nextEntry ?: break
          if (entry.isDirectory || !entry.name.endsWith(".opf", ignoreCase = true)) {
            zip.closeEntry()
            continue
          }

          val opf = zip.readBoundedText(MAX_OPF_BYTES)
          zip.closeEntry()
          return parseOpf(opf)
        }
        EpubMetadata.EMPTY
      }
    } catch (e: Exception) {
      // A book we cannot read metadata from is still a book we can send.
      EpubMetadata.EMPTY
    }
  }

  /**
   * Pulls dc:title and dc:creator out of package XML.
   *
   * Deliberately tolerant rather than a real parse: namespace prefixes vary, attributes appear
   * in any order, and a malformed OPF should cost an empty field rather than an exception on a
   * file the user can otherwise send perfectly well.
   */
  fun parseOpf(opf: String): EpubMetadata {
    val title = firstTag(opf, "title")
    val author = firstTag(opf, "creator")
    return EpubMetadata(title, author)
  }

  private fun firstTag(xml: String, localName: String): String {
    val pattern = Regex(
      """<(?:\w+:)?$localName\b[^>]*>(.*?)</(?:\w+:)?$localName>""",
      setOf(RegexOption.DOT_MATCHES_ALL, RegexOption.IGNORE_CASE),
    )
    val raw = pattern.find(xml)?.groupValues?.get(1) ?: return ""
    return decodeEntities(raw).trim()
  }

  private fun decodeEntities(value: String): String =
    value
      .replace("&lt;", "<")
      .replace("&gt;", ">")
      .replace("&quot;", "\"")
      .replace("&apos;", "'")
      .replace("&#39;", "'")
      .replace("&amp;", "&")

  private fun InputStream.readBoundedText(limit: Int): String {
    val buffer = ByteArray(16 * 1024)
    val out = StringBuilder()
    var total = 0
    while (total < limit) {
      val read = read(buffer)
      if (read <= 0) break
      out.append(String(buffer, 0, read, Charsets.UTF_8))
      total += read
    }
    return out.toString()
  }
}
