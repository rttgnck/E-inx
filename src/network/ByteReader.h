#pragma once

/**
 * @file ByteReader.h
 * @brief Public interface and types for ByteReader.
 */

#include <cstddef>

namespace inx {

/**
 * A pull source of bytes, shaped to what ArduinoJson's default `Reader` calls:
 * `read()` for one byte and `readBytes()` for a run, with -1 and a short count
 * meaning end of input.
 *
 * It exists so a response can be parsed as it arrives instead of being held in
 * memory first. Agent Island's `/api/state` is the reason: it carries every
 * session's whole activity feed, up to eighty entries of up to six thousand
 * characters each, and this device has no PSRAM to hold that in. Handed to the
 * parser with a filter, the payload is discarded as it streams and only the
 * handful of fields the panel draws is ever allocated.
 */
struct ByteReader {
  virtual ~ByteReader() = default;
  virtual int read() = 0;
  virtual size_t readBytes(char* buffer, size_t length) = 0;
};

}  // namespace inx
