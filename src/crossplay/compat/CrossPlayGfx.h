#pragma once

/**
 * @file CrossPlayGfx.h
 * @brief CrossPoint's flat renderer calls, forwarded onto E-inx's sub-renderers.
 *
 * The ported apps draw their own play surfaces — a chess board, two battleship
 * grids, a nonogram — straight to the renderer rather than through FreeInkUI,
 * because sixty-four squares would be sixty-four interactions. That drawing code
 * says `renderer.fillRect(...)`, CrossPoint's flat spelling; E-inx's renderer
 * splits the same operations across `renderer.rectangle`, `renderer.text` and so
 * on.
 *
 * Rather than edit fifty call sites per app — which would make every future
 * upstream diff unreadable — `CrossPlayGfx` wraps a `GfxRenderer` and offers the
 * flat names. Ported activities hold one of these as `renderer`, deliberately
 * shadowing `Activity::renderer`, so the app sources are unchanged. It converts
 * implicitly back to `const GfxRenderer&`, which is what everything else in the
 * port (`toybox::makeTarget`, `toybox::blit1bpp`) takes.
 *
 * This is a forwarding layer and nothing else: no call here does arithmetic that
 * the renderer would not have done, except where E-inx's primitive is genuinely
 * less capable, and each of those is commented.
 */

#include <GfxRenderer.h>

#include <string>

/**
 * @brief CrossPoint's four-level colour, as the apps spell it.
 *
 * E-inx has three fill tones, so LightGray and DarkGray both land on Gray — the
 * same collapse EInxDrawTarget makes, and for the same reason. Kept as four
 * values because the app sources name all four and flattening them at the call
 * site would be a change to code we want to keep diffable.
 */
enum class CrossPlayColor : uint8_t { Clear, White, LightGray, DarkGray, Black };

inline constexpr CrossPlayColor Clear = CrossPlayColor::Clear;
inline constexpr CrossPlayColor White = CrossPlayColor::White;
inline constexpr CrossPlayColor LightGray = CrossPlayColor::LightGray;
inline constexpr CrossPlayColor DarkGray = CrossPlayColor::DarkGray;
inline constexpr CrossPlayColor Black = CrossPlayColor::Black;

class CrossPlayGfx {
 public:
  explicit CrossPlayGfx(GfxRenderer& renderer) : gfx_(renderer) {}

  /**
   * @brief Everything outside the flat surface takes the real renderer.
   *
   * One conversion, not two. A second `operator GfxRenderer&()` made every
   * `toybox::makeTarget(renderer)` ambiguous, and nothing in the port needs a
   * mutable reference: the renderer's own drawing methods are all const, and
   * the two that are not (setOrientation) are wrapped above.
   */
  operator const GfxRenderer&() const { return gfx_; }
  const GfxRenderer& raw() const { return gfx_; }

  /**
   * @brief The renderer as a mutable reference.
   *
   * A handful of firmware APIs take `GfxRenderer&` rather than `const&` — the
   * WiFi picker's constructor, the SD font loaders. Const does not propagate
   * through a reference member, so a const CrossPlayGfx can still hand one out;
   * it is a named accessor rather than a second conversion operator because a
   * second conversion made every `makeTarget(renderer)` ambiguous.
   */
  GfxRenderer& writable() const { return gfx_; }

  // --- pass-through --------------------------------------------------------

  int getScreenWidth() const { return gfx_.getScreenWidth(); }
  int getScreenHeight() const { return gfx_.getScreenHeight(); }
  void clearScreen(uint8_t color = 0xFF) const { gfx_.clearScreen(color); }
  void displayBuffer(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH) const { gfx_.displayBuffer(mode); }
  void setOrientation(GfxRenderer::Orientation o) const { const_cast<GfxRenderer&>(gfx_).setOrientation(o); }
  GfxRenderer::Orientation getOrientation() const { return gfx_.getOrientation(); }
  void drawPixel(int x, int y, bool state = true) const { gfx_.drawPixel(x, y, state); }

  // --- rectangles ----------------------------------------------------------

  void fillRect(int x, int y, int width, int height, bool state = true) const {
    gfx_.rectangle.fill(x, y, width, height, state);
  }

  void fillRectDither(int x, int y, int width, int height, CrossPlayColor color) const {
    gfx_.rectangle.fill(x, y, width, height, toneFor(color));
  }

  void drawRect(int x, int y, int width, int height, bool state = true) const {
    gfx_.rectangle.render(x, y, width, height, state);
  }

  /**
   * @brief A rectangle outline `lineWidth` pixels thick.
   *
   * E-inx's primitive draws a one-pixel outline, so thickness is nested rects
   * insetting from the caller's edge. The outer edge stays where it was asked
   * for, which is what every layout using this to frame a board assumes.
   */
  void drawRect(int x, int y, int width, int height, int lineWidth, bool state) const {
    for (int i = 0; i < lineWidth; ++i) {
      const int w = width - 2 * i;
      const int h = height - 2 * i;
      if (w <= 0 || h <= 0) break;
      gfx_.rectangle.render(x + i, y + i, w, h, state);
    }
  }

  /**
   * @brief A filled rounded rectangle.
   *
   * The radius is advisory: E-inx derives it from the box (min(w,h)/10, or /20
   * when subtle). Callers here pass radius to mean "a circle" (radius = size/2)
   * or "a soft corner" (2-3px), and the two are told apart by whether the radius
   * reaches half the box.
   */
  void fillRoundedRect(int x, int y, int width, int height, int cornerRadius, CrossPlayColor color) const {
    const int minSide = width < height ? width : height;
    const bool subtle = cornerRadius * 2 < minSide / 2;
    gfx_.rectangle.fill(x, y, width, height, toneFor(color), /*rounded=*/cornerRadius > 0, subtle);
  }

  void drawRoundedRect(int x, int y, int width, int height, int lineWidth, int cornerRadius, bool state) const {
    for (int i = 0; i < lineWidth; ++i) {
      const int w = width - 2 * i;
      const int h = height - 2 * i;
      if (w <= 0 || h <= 0) break;
      gfx_.rectangle.render(x + i, y + i, w, h, state, /*rounded=*/cornerRadius > 0);
    }
  }

  // --- lines and text ------------------------------------------------------

  void drawLine(int x1, int y1, int x2, int y2, bool state = true) const { gfx_.line.render(x1, y1, x2, y2, state); }

  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return gfx_.text.getWidth(fontId, text, style);
  }

  int getTextHeight(int fontId) const { return gfx_.text.getHeight(fontId); }
  int getLineHeight(int fontId) const { return gfx_.text.getLineHeight(fontId); }

  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    gfx_.text.render(fontId, x, y, text, black, style);
  }

  std::string truncatedText(int fontId, const char* text, int maxWidth,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) const {
    return gfx_.text.truncate(fontId, text, maxWidth, style);
  }

 private:
  GfxRenderer& gfx_;

  static int toneFor(const CrossPlayColor color) {
    switch (color) {
      case CrossPlayColor::Black:
        return static_cast<int>(GfxRenderer::FillTone::Ink);
      case CrossPlayColor::LightGray:
      case CrossPlayColor::DarkGray:
        return static_cast<int>(GfxRenderer::FillTone::Gray);
      case CrossPlayColor::White:
      case CrossPlayColor::Clear:
      default:
        return static_cast<int>(GfxRenderer::FillTone::Paper);
    }
  }
};
