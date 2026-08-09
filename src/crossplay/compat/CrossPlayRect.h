#pragma once

/**
 * @file CrossPlayRect.h
 * @brief The plain integer rect CrossPlay's apps pass around.
 *
 * Upstream gets this from `components/themes/BaseTheme.h` by way of
 * `components/UITheme.h` — CrossPoint's theme header, which drags in the whole
 * reader theme system for a struct with four ints in it. E-inx has its own,
 * unrelated UiTheme and no global `Rect` at all.
 *
 * So the four ints live here. This is deliberately NOT `freeink::ui::Rect`:
 * that one is int16_t and belongs to the SDK's layout types, and the apps use
 * this one for their own geometry (grid origins, body slots) where the widths
 * are plain screen arithmetic.
 */

struct Rect {
  int x;
  int y;
  int width;
  int height;

  explicit Rect(int x = 0, int y = 0, int width = 0, int height = 0) : x(x), y(y), width(width), height(height) {}
};
