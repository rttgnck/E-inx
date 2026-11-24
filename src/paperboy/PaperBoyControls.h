#pragma once
#include <stdint.h>
namespace PaperBoy {
// Power is a modifier: +Confirm = Start, +Up = Select. These three
// buttons occupy independent inputs, so Start+Select is a detectable chord.
struct Controls {
  bool powerWasDown = false, powerUsed = false, startPulse = false;
  uint32_t powerSince = 0, pulseSince = 0;
  uint8_t sample(bool back, bool confirm, bool left, bool right, bool up, bool down, bool power, uint32_t now) {
    if (power && !powerWasDown) {
      powerSince = now;
      powerUsed = false;
    }
    if (power && (confirm || up || down || left || right || back)) powerUsed = true;
    if (!power && powerWasDown && !powerUsed && uint32_t(now - powerSince) < 700) {
      startPulse = true;
      pulseSince = now;
    }
    powerWasDown = power;
    if (startPulse && uint32_t(now - pulseSince) >= 150) startPulse = false;
    return mask(back, confirm, left, right, up, down, power) | (startPulse ? 8 : 0);
  }
  uint32_t chordSince = 0;
  bool chordActive = false;
  uint8_t mask(bool back, bool confirm, bool left, bool right, bool up, bool down, bool power) const {
    if (power) return (confirm ? 8 : 0) | (up ? 4 : 0);
    return (confirm ? 1 : 0) | (back ? 2 : 0) | (right ? 16 : 0) | (left ? 32 : 0) | (up ? 64 : 0) | (down ? 128 : 0);
  }
  bool exitHeld(uint8_t buttons, uint32_t now) {
    if ((buttons & 12) != 12) {
      chordActive = false;
      return false;
    }
    if (!chordActive) {
      chordActive = true;
      chordSince = now;
    }
    return uint32_t(now - chordSince) >= 3000;
  }
};
inline bool ink(uint8_t shade, int x, int y) {
  static constexpr uint8_t bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  return bayer[y & 3][x & 3] < (shade * 16 + 1) / 3;
}
}  // namespace PaperBoy
