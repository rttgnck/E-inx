#pragma once

/**
 * @file ToyboxScreen.h
 * @brief Toybox screens, the way the SDK intends them to be written.
 *
 * A screen is a free function taking a `toybox::Screen&` and a plain model
 * struct. It touches no renderer, no Activity and no storage, which buys two
 * things:
 *
 *   - `freeink::ui::Screen` substitutes theme tokens into every component it
 *     builds (title style, side padding, row height, row gap, selection style).
 *     Calling the components directly opts out of that.
 *   - screens stay freestanding. FreeInkUI is C++17 with no Arduino and no
 *     renderer, so a screen builder compiles against a fake draw target.
 *
 * Ported from CrossPlay's ui/ToyboxScreen.h essentially unchanged: this header
 * only ever touched the SDK and the tokens, which is exactly why it survived the
 * move between forks untouched while the renderer-facing files did not.
 *
 * Activities keep what genuinely needs hardware: reading state, filling in the
 * model, and drawing their own play surface into the body rect.
 */

#include <FreeInkApp.h>
#include <FreeInkUI.h>
#include <FreeInkUIIcon.h>
#include <Icon.h>

#include "ToyboxTokens.h"

namespace toybox {

/**
 * @brief How many controls one screen may register.
 *
 * Sized for the largest in the fork: Connections' sixteen tiles plus its three
 * action buttons, with room to grow. Raise this, do not trim a screen to fit it
 * — a screen that overflows silently drops controls, and on a button-driven
 * device a dropped control is one the focus ring cannot reach at all.
 */
constexpr size_t kMaxInteractions = 24;

using Interactions = freeink::ui::InteractionBuffer<kMaxInteractions>;

namespace detail {
/**
 * @brief Storage for the two things every screen hands in as a temporary.
 *
 * `freeink::ui::Frame` keeps a `const DeviceContext&`, and `target.deviceContext()`
 * returns by value. Writing the obvious
 *
 *     toybox::Frame frame(target, target.deviceContext(), noInput, interactions);
 *
 * binds the reference to a temporary that dies at the end of its own statement,
 * and every layout read afterwards is undefined. A base is initialised before
 * the bases listed after it, so holding the value in a base of the wrapper gives
 * the SDK object something that outlives it.
 */
struct OwnedDevice {
  freeink::ui::DeviceContext ownedDevice;
};
}  // namespace detail

/**
 * @brief A frame that owns its device context.
 *
 * The InputSnapshot is still held by reference, which is correct: every caller
 * passes a named local, and copying it would break a frame whose input is filled
 * in after construction.
 */
class Frame : private detail::OwnedDevice, public freeink::ui::Frame<kMaxInteractions> {
 public:
  Frame(freeink::ui::DrawTarget& target, const freeink::ui::DeviceContext& device,
        const freeink::ui::InputSnapshot& input, Interactions& interactions,
        freeink::ui::AssetResolver* assets = nullptr)
      : detail::OwnedDevice{device},
        freeink::ui::Frame<kMaxInteractions>(target, OwnedDevice::ownedDevice, input, interactions, assets) {}
};

/**
 * @brief A screen drawn in the Toybox palette.
 *
 * The theme is not a constructor parameter in the common case because it never
 * varied. It is a function-local static (see ToyboxTokens.h), so referring to it
 * straight cannot dangle, and the 2712-byte copy it replaces is 2712 bytes of
 * stack this device does not have to find.
 */
class Screen : public freeink::ui::Screen<kMaxInteractions> {
 public:
  /** The shared palette. What most screens want. */
  explicit Screen(freeink::ui::Frame<kMaxInteractions>& frame)
      : freeink::ui::Screen<kMaxInteractions>(frame, themeTokens()) {}

  /**
   * @brief A palette of the caller's own, for screens that alter the band.
   *
   * `theme` is referred to, not copied, so it has to outlive the screen: a named
   * local in the same render() does, which is every real use.
   */
  Screen(freeink::ui::Frame<kMaxInteractions>& frame, const freeink::ui::ThemeTokens& theme)
      : freeink::ui::Screen<kMaxInteractions>(frame, theme) {}

  /** Passing a temporary theme is the dangling bug above; fail to compile instead. */
  Screen(freeink::ui::Frame<kMaxInteractions>&, freeink::ui::ThemeTokens&&) = delete;
};

/** Every icon this fork draws, at the one size the generator emits. */
constexpr int16_t kIconSize = 32;

/**
 * @brief An icon at the right edge of row `index` in a list band.
 *
 * The FreeInkUI list component only draws icons on the left, which indents every
 * label and leaves the text starting at a different x from the header above it.
 * Right-aligned, the icons share one axis and the labels stay flush.
 *
 * `selected` picks the ink: the selected row is filled black, so its icon has to
 * be paper.
 */
inline void iconAtRowRight(Screen& screen, const freeink::ui::Rect& band, const int index, const freeink::Icon& icon,
                           const bool selected) {
  namespace fui = freeink::ui;
  const int16_t rowHeight = screen.theme().rowHeight;
  const int16_t rowGap = screen.theme().listRowGap;
  const int16_t rowY = static_cast<int16_t>(band.y + index * (rowHeight + rowGap));
  const fui::Rect where = fui::makeRect(static_cast<int16_t>(band.x + band.width - kIconSize - kGutter * 2),
                                        static_cast<int16_t>(rowY + (rowHeight - kIconSize) / 2), kIconSize, kIconSize);
  screen.target().bitmap(where, fui::bitmapFromIcon(icon), fui::BitmapMode::Contain,
                         fui::Paint::solid(selected ? fui::Color::White : fui::Color::Black));
}

}  // namespace toybox
