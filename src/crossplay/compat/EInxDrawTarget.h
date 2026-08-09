#pragma once

/**
 * @file EInxDrawTarget.h
 * @brief FreeInkUI DrawTarget backed by E-inx's GfxRenderer.
 *
 * This is the port's load-bearing file. The FreeInk SDK ships
 * FreeInkUIGfxRenderer.h, which binds FreeInkUI to CrossPoint's GfxRenderer;
 * E-inx's GfxRenderer is the same class after a different two years, split into
 * sub-renderers (`renderer.text`, `renderer.rectangle`, ...) instead of one flat
 * surface. So the SDK's adapter is dropped on vendoring and this replaces it.
 *
 * DrawTarget is seven pure virtuals. Everything CrossPlay's apps draw goes
 * through them, which is why 53k lines of app code can move across on the back
 * of one file.
 *
 * Three capabilities E-inx's renderer does not have, and what is done instead:
 *
 *   - Arbitrary corner radius. `rectangle.fill`/`render` take a `rounded` bool
 *     and derive the radius from the box (min(w,h)/10, or /20 when `subtle`).
 *     A requested radius is therefore honoured as "rounded, roughly this much":
 *     small radii map to subtle, larger ones to the standard curve. Per-corner
 *     selection has no equivalent at all and is ignored — every corner rounds.
 *   - Stroke and line width. Neither primitive takes one, so width is drawn as
 *     nested rects / parallel lines.
 *   - Four grey levels. E-inx has three fill tones (Paper, Ink, Gray), so
 *     LightGray and DarkGray both land on Gray. On a 1-bit panel with Bayer
 *     dithering this costs the distinction between two dither densities, which
 *     Toybox uses only for de-emphasis.
 *
 * None of these change what a screen *means*, and all three are visible only
 * side by side with an X4 Pro. They are listed because a later reader will
 * otherwise think they were missed.
 */

#include <FreeInkUI.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <string>
#include <vector>

namespace einxui {

/** Greedy word wrap over E-inx's text measurement. Fills FreeInkUI's gap. */
std::vector<std::string> wrapText(const GfxRenderer& renderer, int fontId, const char* text, int maxWidth,
                                  int maxLines, EpdFontFamily::Style style);

/**
 * @brief FreeInkUI's DrawTarget over E-inx's GfxRenderer.
 *
 * FreeInkUI addresses fonts as three small slots (small/body/title); the
 * firmware binds each slot to a real font id, so a screen builder never names
 * an E-inx font and the same builder draws in whatever face the app picked.
 */
class EInxDrawTarget final : public freeink::ui::DrawTarget {
 public:
  using FontId = freeink::ui::FontId;

  static constexpr FontId FONT_SMALL = 0;
  static constexpr FontId FONT_BODY = 1;
  static constexpr FontId FONT_TITLE = 2;
  static constexpr size_t FONT_SLOTS = 3;

  explicit EInxDrawTarget(const GfxRenderer& renderer) : renderer_(renderer) {
    for (size_t i = 0; i < FONT_SLOTS; ++i) fonts_[i] = 0;
  }

  void setFont(const FontId slot, const int gfxFontId) {
    if (slot < FONT_SLOTS) fonts_[slot] = gfxFontId;
  }

  freeink::ui::DeviceContext deviceContext() const {
    freeink::ui::DeviceContext device;
    device.width = static_cast<int16_t>(renderer_.getScreenWidth());
    device.height = static_cast<int16_t>(renderer_.getScreenHeight());
    // The two Orientation enums declare the same four values in the same order,
    // both forks having inherited them from the same ancestor.
    device.orientation = static_cast<freeink::ui::Orientation>(renderer_.getOrientation());
    device.touchOrientation = freeink::ui::touchOrientationFor(device.orientation);
    // The X3 is buttons and nothing else. Every ported screen navigates through
    // FreeInkUI's focus ring rather than a tap; see CrossPlayFocus.h.
    device.hasTouch = false;
    device.hasButtons = true;
    return device;
  }

  freeink::ui::Size measureText(const FontId font, const char* text,
                                const freeink::ui::TextStyle style) const override {
    if (!text) return {};
    const int fontId = gfxFont(font);
    return freeink::ui::Size{
        static_cast<int16_t>(renderer_.text.getWidth(fontId, text, fontStyle(style))),
        static_cast<int16_t>(renderer_.text.getLineHeight(fontId))};
  }

  int16_t lineHeight(const FontId font) const override {
    return static_cast<int16_t>(renderer_.text.getLineHeight(gfxFont(font)));
  }

  void fill(const freeink::ui::Rect rect, const freeink::ui::Paint paint, const uint8_t radius = 0,
            const uint8_t /*corners*/ = freeink::ui::CornersAll) override;

  void stroke(const freeink::ui::Rect rect, const freeink::ui::Paint paint, const uint8_t width,
              const uint8_t radius = 0, const uint8_t /*corners*/ = freeink::ui::CornersAll) override;

  void line(const freeink::ui::Point from, const freeink::ui::Point to, const uint8_t width,
            const freeink::ui::Paint paint) override;

  void triangle(const freeink::ui::Point a, const freeink::ui::Point b, const freeink::ui::Point c,
                const freeink::ui::Paint paint) override;

  void text(const freeink::ui::Rect rect, const char* text, const freeink::ui::TextStyle style) override;

  void bitmap(const freeink::ui::Rect rect, const freeink::ui::BitmapRef bitmap, const freeink::ui::BitmapMode mode,
              const freeink::ui::Paint foreground = freeink::ui::Paint::solid(freeink::ui::Color::Black),
              const freeink::ui::Rotation rotation = freeink::ui::Rotation::None) override;

 private:
  const GfxRenderer& renderer_;
  int fonts_[FONT_SLOTS];

  int gfxFont(const FontId slot) const { return slot < FONT_SLOTS ? fonts_[slot] : fonts_[FONT_BODY]; }

  /**
   * @brief FreeInkUI colour to an E-inx fill tone.
   *
   * Two greys collapse into one; see the file header. Transparent is caller-
   * filtered before it reaches here, so it maps to Paper defensively rather
   * than meaningfully.
   */
  static int fillTone(const freeink::ui::Color color) {
    switch (color) {
      case freeink::ui::Color::Black:
        return static_cast<int>(GfxRenderer::FillTone::Ink);
      case freeink::ui::Color::LightGray:
      case freeink::ui::Color::DarkGray:
        return static_cast<int>(GfxRenderer::FillTone::Gray);
      case freeink::ui::Color::White:
      case freeink::ui::Color::Transparent:
      default:
        return static_cast<int>(GfxRenderer::FillTone::Paper);
    }
  }

  /**
   * @brief Whether a requested radius should use the renderer's tighter curve.
   *
   * E-inx picks the radius from the box, so the only control a caller has is
   * which of the two curves it gets. Anything up to 4px reads as a softened
   * corner rather than a rounded one, which is what `subtle` draws.
   */
  static bool subtleFor(const uint8_t radius) { return radius > 0 && radius <= 4; }

  static EpdFontFamily::Style fontStyle(const freeink::ui::TextStyle& style) {
    return style.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  }
};

}  // namespace einxui
