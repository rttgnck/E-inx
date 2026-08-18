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

class ChunkSizeTest {

  @Test
  fun `an MTU of 517 does not produce an illegal 514 byte write`() {
    // The GATT attribute limit is 512 whatever the MTU says. Android throws on 514.
    assertEquals(512, chunkSizeForMtu(517))
  }

  @Test
  fun `smaller MTUs keep their full payload`() {
    assertEquals(20, chunkSizeForMtu(23))
    assertEquals(182, chunkSizeForMtu(185))
    assertEquals(244, chunkSizeForMtu(247))
  }

  @Test
  fun `a nonsense MTU never yields a write smaller than the BLE minimum`() {
    assertEquals(20, chunkSizeForMtu(0))
    assertEquals(20, chunkSizeForMtu(5))
  }
}

class StartMetadataTest {

  @Test
  fun `title and author are sent when set`() {
    val json = JSONObject(
      String(BleProtocol.startCommand("Dune.epub", 10L, 1L, "Dune", "Frank Herbert"))
    )
    assertEquals("Dune", json.getString("title"))
    assertEquals("Frank Herbert", json.getString("author"))
  }

  @Test
  fun `blank metadata is omitted so the reader keeps the book's own`() {
    val json = JSONObject(String(BleProtocol.startCommand("Dune.epub", 10L, 1L, "", "   ")))
    assertFalse(json.has("title"))
    assertFalse(json.has("author"))
  }

  @Test
  fun `metadata is trimmed`() {
    val json = JSONObject(String(BleProtocol.startCommand("Dune.epub", 10L, 1L, "  Dune  ", " Herbert ")))
    assertEquals("Dune", json.getString("title"))
    assertEquals("Herbert", json.getString("author"))
  }

  @Test
  fun `the protocol version is unchanged, so older readers still accept the start`() {
    val json = JSONObject(String(BleProtocol.startCommand("Dune.epub", 10L, 1L, "Dune", "Herbert")))
    assertEquals(1, json.getInt("protocol"))
  }
}
