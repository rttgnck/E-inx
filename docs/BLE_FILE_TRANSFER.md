# Bluetooth LE file transfer

How a book gets from a phone onto an E-inx reader's SD card.

This is the contract between three files. Change one and you change all of them:

| Side | File |
| --- | --- |
| Reader (receiver) | `src/network/BluetoothTransferServer.cpp` |
| Android (sender) | `android/app/src/main/java/com/einx/send/BleProtocol.kt` |
| Spec | this document |

The transport is Bluetooth Low Energy with a custom GATT service. There is no Bluetooth
Classic, no OBEX, and no pairing — a reader only advertises the service while the user is
sitting on **Device Connections → Bluetooth Transfer**, and the radio is shut down the moment
they leave that screen.

## Protocol version

    protocol = 1

The sender puts its version in `START`. A reader that does not implement that version answers
`ERR_PROTOCOL` and takes nothing.

## Service and characteristics

    Service   e1780001-5b41-4d2e-9a63-7c8f1b0d4e21

    CONTROL   e1780002-5b41-4d2e-9a63-7c8f1b0d4e21   write
    DATA      e1780003-5b41-4d2e-9a63-7c8f1b0d4e21   write, write-without-response
    STATUS    e1780004-5b41-4d2e-9a63-7c8f1b0d4e21   read, notify

The service UUID is in the advertisement, so a sender can filter its scan and show only
readers that are actually waiting for a book.

`CONTROL` and `STATUS` carry UTF-8 JSON. `DATA` carries raw file bytes and nothing else.

## The exchange

```
Android                          Reader

connect ───────────────────────▶
        ◀────────────────────── STATUS ready

START (name, size, crc32) ─────▶ validate, open /Books/.<name>.part
        ◀────────────────────── STATUS receiving      (or STATUS error, and stop)

DATA ══════════════════════════▶ write straight to SD
DATA ══════════════════════════▶ write straight to SD
        ◀────────────────────── STATUS progress
DATA ══════════════════════════▶ write straight to SD

FINISH ────────────────────────▶ verify size, verify CRC32, rename .part → /Books/<name>
        ◀────────────────────── STATUS complete       (or STATUS error, and delete .part)
```

Neither side ever holds the whole book in memory. The reader keeps a 16 KB ring buffer
between the radio and the card; the phone reads one chunk at a time from the content Uri.

### Flow control

`DATA` uses write-without-response, and the sender waits for its stack to acknowledge each
write before reading the next chunk. That acknowledgement is the whole of the flow control:
an SD write is slower than the radio, and a sender that does not wait will overrun the
receiver's buffer. On the reader side a `DATA` write that finds the ring full blocks until
there is room, which back-pressures the link layer; if the card has not caught up within
4 seconds the transfer is failed with `ERR_STORAGE` rather than silently dropping bytes.

Chunk size is the negotiated ATT MTU minus 3. The sender asks for MTU 517.

## Messages

### CONTROL → reader

**START**

```json
{
  "cmd": "start",
  "protocol": 1,
  "name": "Dune.epub",
  "size": 2184512,
  "crc32": "a37159c2"
}
```

`crc32` is lowercase hex, zero-padded to eight digits, of the complete file. It is sent as
text so neither side has to agree on how an unsigned 32-bit number is spelled in JSON.

`cmd` may be omitted on START; a CONTROL message carrying a `name` is understood as a START.

**FINISH**

```json
{ "cmd": "finish" }
```

Sent after the last `DATA` write. The reader answers only once every buffered byte is on the
card and both checks have passed.

**ABORT**

```json
{ "cmd": "abort" }
```

Cancels immediately. The reader closes the file, deletes the `.part`, and returns to waiting.

### STATUS → sender

```json
{ "st": "ready",     "proto": 1, "name": "E-inx X3 A31F", "maxSize": 67108864 }
{ "st": "receiving", "name": "Dune.epub", "total": 2184512 }
{ "st": "progress",  "recv": 1572864, "total": 2184512 }
{ "st": "complete",  "name": "Dune.epub", "size": 2184512 }
{ "st": "error",     "code": "ERR_CRC", "msg": "The file did not survive the transfer intact" }
```

`progress` is rate-limited to roughly one message per 64 KB or per second, whichever is
less frequent — status traffic competes with the data for the same connection events.

A sender must tolerate an unrecognised `st` value rather than treating it as an error.

## What the reader validates

Before a single byte is written, on `START`:

| Check | Rule |
| --- | --- |
| Protocol | `protocol` must equal 1 |
| Active transfer | no other transfer may be in progress |
| File name | non-empty, ≤ 96 characters, no `/` `\` `:` `*` `?` `"` `<` `>` `|`, no control characters, must not start with `.` or a space, must not contain `..` |
| Path traversal | the name is used as a bare file name inside `/Books` and is never joined as a path |
| Extension | `.epub`, `.txt`, `.xtc` or `.xtch`, case-insensitive |
| File size | greater than zero and at most 64 MB |
| Duplicate | `/Books/<name>` must not already exist — an existing book is never overwritten |

While receiving:

| Check | Rule |
| --- | --- |
| Overflow | the total of all `DATA` writes may not exceed the promised `size` |
| Storage | every SD write must commit in full |

On `FINISH`:

| Check | Rule |
| --- | --- |
| Size | bytes written and the on-card file size must both equal `size` |
| CRC32 | the checksum computed while writing must equal the promised `crc32` |
| Duplicate | `/Books/<name>` is re-checked before the rename |

CRC32 is the IEEE 802.3 polynomial — the reflected `0xEDB88320` form with an initial and
final value of `0xFFFFFFFF`. This is what `java.util.zip.CRC32`, zip, and gzip produce, so
the sender computes it with the platform class and the reader computes it incrementally as
it writes.

## Partial files

An arriving book is written to

    /Books/.Dune.epub.part

and is renamed to `/Books/Dune.epub` only after both checks pass. Because the temporary name
starts with a dot and does not end in a supported extension, an interrupted transfer can
never show up in the library.

Every failure path — rejected START, connection dropped, cancelled from either end, bad CRC,
short file, SD write failure, leaving the screen — closes the file, deletes the `.part`, resets
the transfer state, and goes back to waiting. Leaving the Bluetooth Transfer screen also
tears the BLE stack down completely.

## Error codes

| Code | Meaning |
| --- | --- |
| `ERR_PROTOCOL` | Unsupported protocol version, or a malformed START field |
| `ERR_JSON` | CONTROL payload was not valid JSON |
| `ERR_BUSY` | A transfer is already in progress |
| `ERR_NAME` | File name rejected |
| `ERR_EXT` | Unsupported file type |
| `ERR_SIZE` | Size is zero or above the 64 MB limit |
| `ERR_EXISTS` | A book with that name is already in the library |
| `ERR_STATE` | FINISH arrived with no transfer open |
| `ERR_OVERFLOW` | Sender sent more data than it promised |
| `ERR_STORAGE` | SD card could not be written, or could not keep up |
| `ERR_TRUNCATED` | The file arrived incomplete |
| `ERR_CRC` | Checksum mismatch |
| `ERR_ABORTED` | Cancelled from the phone or the reader |
| `ERR_DISCONNECTED` | The link dropped mid-transfer |

These are also raised by the Android app for its own failures, which never reach the reader:
`ERR_CONNECT`, `ERR_SERVICE`, `ERR_WRITE`, `ERR_READ`, `ERR_TIMEOUT`, `ERR_PERMISSION`,
`ERR_UNKNOWN`.

## Advertised name

    E-inx X3 A31F

The suffix is the low two bytes of the chip's factory MAC, in uppercase hex. It exists so a
user with two readers in the room can tell them apart; it is not a secret and not an identity.

## Security posture

There is no pairing, no bonding and no authentication. Anyone in Bluetooth range of a reader
that is sitting on the Bluetooth Transfer screen can send it a book, and the protections are
the validation table above — a sender cannot choose a path, overwrite an existing book, exhaust
memory, or leave a partial file behind. The window is the screen: the radio does not exist
before the user opens it or after they leave.
