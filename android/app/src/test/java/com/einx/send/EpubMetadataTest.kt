package com.einx.send

import org.junit.Assert.assertEquals
import org.junit.Test

class EpubMetadataTest {

  @Test
  fun `reads a plain package document`() {
    val opf = """
      <?xml version="1.0"?>
      <package xmlns="http://www.idpf.org/2007/opf" version="3.0">
        <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
          <dc:title>Dune</dc:title>
          <dc:creator>Frank Herbert</dc:creator>
        </metadata>
      </package>
    """.trimIndent()

    val metadata = EpubMetadataReader.parseOpf(opf)

    assertEquals("Dune", metadata.title)
    assertEquals("Frank Herbert", metadata.author)
  }

  @Test
  fun `namespace prefix is not assumed`() {
    val opf = "<metadata><title>Dune</title><creator>Frank Herbert</creator></metadata>"
    val metadata = EpubMetadataReader.parseOpf(opf)
    assertEquals("Dune", metadata.title)
    assertEquals("Frank Herbert", metadata.author)
  }

  @Test
  fun `attributes on the element do not hide the text`() {
    val opf = """<dc:title id="t1" opf:file-as="Dune">Dune</dc:title>"""
    assertEquals("Dune", EpubMetadataReader.parseOpf(opf).title)
  }

  @Test
  fun `entities are decoded`() {
    val opf = "<dc:title>Tom &amp; Jerry &lt;the book&gt;</dc:title><dc:creator>O&apos;Brien</dc:creator>"
    val metadata = EpubMetadataReader.parseOpf(opf)
    assertEquals("Tom & Jerry <the book>", metadata.title)
    assertEquals("O'Brien", metadata.author)
  }

  @Test
  fun `a title spanning lines is collapsed to its text`() {
    val opf = "<dc:title>\n  Dune\n</dc:title>"
    assertEquals("Dune", EpubMetadataReader.parseOpf(opf).title)
  }

  @Test
  fun `the first title wins when a package names several`() {
    val opf = "<dc:title>Dune</dc:title><dc:title>Dune Messiah</dc:title>"
    assertEquals("Dune", EpubMetadataReader.parseOpf(opf).title)
  }

  @Test
  fun `missing fields come back blank rather than throwing`() {
    val metadata = EpubMetadataReader.parseOpf("<package><metadata/></package>")
    assertEquals("", metadata.title)
    assertEquals("", metadata.author)
    assertEquals(true, metadata.isEmpty)
  }

  @Test
  fun `nonsense input is survivable`() {
    assertEquals(EpubMetadata.EMPTY, EpubMetadataReader.parseOpf("not xml at all"))
    assertEquals(EpubMetadata.EMPTY, EpubMetadataReader.parseOpf(""))
  }

  @Test
  fun `a stream that is not a zip yields empty rather than failing the send`() {
    val metadata = EpubMetadataReader.read("this is a plain text file".byteInputStream())
    assertEquals(EpubMetadata.EMPTY, metadata)
  }

  @Test
  fun `reads the opf out of a real zip`() {
    val opf = "<dc:title>Dune</dc:title><dc:creator>Frank Herbert</dc:creator>"
    val zip = java.io.ByteArrayOutputStream().also { out ->
      java.util.zip.ZipOutputStream(out).use { zos ->
        zos.putNextEntry(java.util.zip.ZipEntry("mimetype"))
        zos.write("application/epub+zip".toByteArray())
        zos.closeEntry()
        zos.putNextEntry(java.util.zip.ZipEntry("OEBPS/content.opf"))
        zos.write(opf.toByteArray())
        zos.closeEntry()
      }
    }.toByteArray()

    val metadata = EpubMetadataReader.read(zip.inputStream())

    assertEquals("Dune", metadata.title)
    assertEquals("Frank Herbert", metadata.author)
  }
}
