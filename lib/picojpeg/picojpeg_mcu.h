#pragma once

// Where pixel (x, y) of the current MCU sits in picojpeg's m_pMCUBufR/G/B.
//
// picojpeg always lays the MCU buffer out as a 2x2 grid of 8x8 blocks (byte offsets 0, 64, 128, 192),
// whatever the MCU's real size. An 8x8 MCU uses block 0, a 16x8 MCU blocks 0 and 64, a 16x16 MCU all four,
// and an 8x16 (H1V2) MCU blocks 0 and 128 (see transformBlock() in picojpeg.c). The block row stride is
// therefore always 2, never MCUWidth / 8: using MCUWidth / 8 reads block 64 for the lower half of an H1V2
// MCU, which picojpeg never writes, and draws every other 8-row strip from stale data.
//
// In reduce (DC-only) mode each block is one pixel, stored at its block's first byte: pass the block
// coordinates times 8.
static inline int pjpegMcuOffset(const int x, const int y) {
  return ((y >> 3) * 2 + (x >> 3)) * 64 + (y & 7) * 8 + (x & 7);
}
