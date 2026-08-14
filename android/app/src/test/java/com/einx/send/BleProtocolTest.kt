package com.einx.send

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class BleProtocolTest {

  @Test
  fun `start command carries the fields the firmware validates`() {
    val json = JSONObject(String(BleProtocol.startCommand("Dune.epub", 2_184_512L, 0xa37159c2L)))

    assertEquals("start", json.getString("cmd"))
    assertEquals(1, json.getInt("protocol"))
    assertEquals("Dune.epub", json.getString("name"))
    assertEquals(2_184_512L, json.getLong("size"))
    assertEquals("a37159c2", json.getString("crc32"))
  }

  @Test
  fun `crc is zero padded so the firmware's hex parse always sees eight digits`() {
    val json = JSONObject(String(BleProtocol.startCommand("a.txt", 1L, 0x0000beefL)))
    assertEquals("0000beef", json.getString("crc32"))
  }

  @Test
  fun `status messages parse into their variants`() {
    val ready = BleProtocol.parseStatus(
      """{"st":"ready","proto":1,"name":"E-inx X3 A31F","maxSize":67108864}""".toByteArray()
    )
    assertTrue(ready is BleProtocol.Status.Ready)
    assertEquals("E-inx X3 A31F", (ready as BleProtocol.Status.Ready).readerName)

    val receiving =
      BleProtocol.parseStatus("""{"st":"receiving","name":"Dune.epub","total":10}""".toByteArray())
    assertEquals(10L, (receiving as BleProtocol.Status.Receiving).total)

    val progress = BleProtocol.parseStatus("""{"st":"progress","recv":5,"total":10}""".toByteArray())
    assertEquals(5L, (progress as BleProtocol.Status.Progress).received)

    val complete =
      BleProtocol.parseStatus("""{"st":"complete","name":"Dune.epub","size":10}""".toByteArray())
    assertEquals("Dune.epub", (complete as BleProtocol.Status.Complete).name)

    val error =
      BleProtocol.parseStatus("""{"st":"error","code":"ERR_CRC","msg":"bad"}""".toByteArray())
    assertEquals("ERR_CRC", (error as BleProtocol.Status.Error).code)
  }

  @Test
  fun `malformed status does not throw`() {
    assertTrue(BleProtocol.parseStatus("not json".toByteArray()) is BleProtocol.Status.Unknown)
    assertTrue(BleProtocol.parseStatus("""{"st":"weather"}""".toByteArray()) is BleProtocol.Status.Unknown)
  }

  @Test
  fun `only the reader's four formats are offered`() {
    assertTrue(BleProtocol.isSupportedFilename("Dune.epub"))
    assertTrue(BleProtocol.isSupportedFilename("Dune.EPUB"))
    assertTrue(BleProtocol.isSupportedFilename("notes.txt"))
    assertTrue(BleProtocol.isSupportedFilename("comic.xtc"))
    assertTrue(BleProtocol.isSupportedFilename("comic.xtch"))

    assertFalse(BleProtocol.isSupportedFilename("Dune.pdf"))
    assertFalse(BleProtocol.isSupportedFilename("Dune.mobi"))
    assertFalse(BleProtocol.isSupportedFilename("Dune"))
  }
}
