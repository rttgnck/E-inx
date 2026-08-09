#pragma once

/**
 * @file NearbyMark.h
 * @brief The PLAY NEARBY mark, and the two link screens the games ask for.
 *
 * Upstream's `link/LinkScreens.h` draws the searching, disconnect and rematch
 * screens and owns this icon. None of that is ported (see SoloLink.h), but the
 * two games' menus reference `linkui::nearbyMark()` and `withOpponentFace()`
 * while drawing rows that are otherwise upstream's exactly — so these two exist
 * to keep those files unedited and diffable.
 *
 * The mark is deliberately kept even though PLAY NEARBY does nothing yet. A row
 * that quietly loses its icon looks like a drawing bug; a row that keeps it and
 * says why it declined looks like a feature that is not built. It is the second
 * one.
 */

#include <Icon.h>

#include "../ui/ToyboxScreen.h"

namespace linkui {

namespace fui = freeink::ui;

/**
 * Two devices with a signal between them, 24x24, Mask1 (bit 0 = ink).
 * Generated rather than hand-typed; the generator is in the commit that added
 * this file.
 */
inline constexpr uint8_t kNearbyBits[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0x80, 0x7E, 0x01,
    0xBF, 0x7E, 0xFD,
    0xA1, 0x7E, 0x85,
    0xA1, 0x7E, 0x85,
    0xA1, 0x7E, 0x85,
    0xA1, 0x7E, 0x85,
    0xA1, 0x66, 0x85,
    0xA1, 0x7E, 0x85,
    0xA1, 0x66, 0x85,
    0xA1, 0x7E, 0x85,
    0xA1, 0x66, 0x85,
    0xA1, 0x7E, 0x85,
    0xBF, 0x7E, 0xFD,
    0xBF, 0x7E, 0xFD,
    0xBF, 0x7E, 0xFD,
    0x80, 0x7E, 0x01,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF,
};

inline const freeink::Icon& nearbyMark() {
  static constexpr freeink::Icon mark{24, 24, 12, kNearbyBits};
  return mark;
}

/**
 * @brief Puts the opponent's face at the left of `band` and returns what is left.
 *
 * Upstream draws a face derived from the opponent's three-word name. There is no
 * opponent here, so this is the branch upstream already has for solo play
 * against the engine: "a null or empty name returns `band` untouched, which is
 * what solo play gets — there is nobody to show."
 */
inline fui::Rect withOpponentFace(toybox::Screen&, const fui::Rect& band, const char*) { return band; }

}  // namespace linkui
