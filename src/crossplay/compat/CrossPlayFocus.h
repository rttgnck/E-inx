#pragma once

/**
 * @file CrossPlayFocus.h
 * @brief Buttons where CrossPlay expects a finger.
 *
 * Every game in CrossPlay is touch-only. Its `loop()` reads a tap and returns
 * early if there was not one, because the X4 Pro it targets has a capacitive
 * digitiser and its author decided a cursor on top of a touchscreen was clutter.
 * The X3 has no touchscreen at all, so a straight port of any of those apps
 * builds, boots, draws correctly, and cannot be played.
 *
 * The fix is smaller than it looks, because FreeInkUI was built for both. Its
 * InteractionBuffer already keeps a focus index, `route()` already handles
 * `focusNext`/`focusPrev`/`confirm`, and `stateFor()` already ORs in
 * StateFocused so the focused control paints highlighted with no change to any
 * screen builder. CrossPlay's activities simply never fill those fields in.
 *
 * So this fills them in. It lives in the compat layer rather than in each app
 * for the obvious reason: every app that follows Solitaire needs exactly this
 * and should not each invent it.
 */

#include <FreeInkUI.h>

#include "system/MappedInputManager.h"

namespace crossplay {

/**
 * @brief Reads the X3's buttons into a FreeInkUI input snapshot.
 *
 * The mapping follows what E-inx's own games already teach: the paging pair
 * walks a list and Confirm acts on it. Up/Down double the paging pair rather
 * than doing something separate, because on a four-button device that is one
 * fewer thing to learn and the directional pair is what a user reaches for
 * first on a grid.
 *
 * Back is deliberately NOT mapped. Every ported app already handles Back itself
 * for leaving the screen, and routing it here too would fire both.
 */
inline freeink::ui::InputSnapshot readFocusInput(const MappedInputManager& mappedInput) {
  freeink::ui::InputSnapshot input;

  if (mappedInput.wasPressed(MappedInputManager::Button::PageBack) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left) ||
      mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    input.focusPrev = true;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::PageForward) ||
      mappedInput.wasPressed(MappedInputManager::Button::Right) ||
      mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    input.focusNext = true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    input.confirm = true;
  }

  return input;
}

/** True when the snapshot carries anything the interaction table would react to. */
inline bool hasFocusInput(const freeink::ui::InputSnapshot& input) {
  return input.focusPrev || input.focusNext || input.confirm;
}

/**
 * @brief Puts focus on the first control that will take it, if nothing has it.
 *
 * A freshly built screen has focus index -1. `route()` handles that correctly
 * for a paging press, but two other things are wrong without this: the screen
 * paints with nothing highlighted, so the first frame gives no clue that there
 * is a cursor at all, and Confirm does nothing until a direction has been
 * pressed once.
 *
 * The focusable test mirrors InteractionBuffer's own private one. Duplicating
 * it is unfortunate; the alternative is a patch to the vendored SDK, which
 * would have to be re-applied on every update.
 */
template <size_t MaxInteractions>
void ensureFocus(freeink::ui::InteractionBuffer<MaxInteractions>& interactions) {
  if (interactions.focusedIndex() >= 0) return;
  const freeink::ui::Interaction* entries = interactions.data();
  for (size_t i = 0; i < interactions.count(); ++i) {
    if (freeink::ui::hasState(entries[i].state, freeink::ui::StateDisabled)) continue;
    if (!freeink::ui::acceptsInput(entries[i].inputMask, freeink::ui::InputFocus)) continue;
    interactions.setFocusedIndex(static_cast<int16_t>(i));
    return;
  }
}

/**
 * @brief The input scheme for a game with a play surface as well as chrome.
 *
 * Solitaire needs only the focus ring, because every one of its controls is a
 * registered interaction. Chess, Battleship and D&Diagrams are not like that:
 * their boards are drawn straight to the renderer and hit-tested from the
 * geometry that drew them, because sixty-four squares would be sixty-four
 * interactions and the buffer holds twenty-four. So those games have two things
 * to point at, and one set of buttons.
 *
 * The split:
 *
 *   direction pad   move the cursor on the board
 *   Confirm         act at the cursor
 *   Prev / Next     step through the chrome buttons instead, showing focus
 *   Confirm (chrome) activate the focused button, and hand the buttons back
 *                   to the board
 *
 * A direction press always returns to the board, so there is no mode to get
 * stuck in: if the cursor is not where you expect, press a direction and it is.
 */
class ChromeFocus {
 public:
  /** True while the chrome owns Confirm. */
  bool active() const { return active_; }

  /** A direction was pressed: the board takes the buttons back. */
  void release() { active_ = false; }

  /**
   * @brief Steps to the next/previous chrome control, entering chrome mode.
   *
   * Returns whatever the table did with it, so a caller can repaint. The first
   * press only enters the mode and lands on the first control, which is what
   * makes the focus highlight appear before anything is activated.
   */
  template <size_t MaxInteractions>
  bool step(freeink::ui::InteractionBuffer<MaxInteractions>& interactions, const bool forward) {
    freeink::ui::InputSnapshot input;
    if (!active_) {
      // Entering: put focus on something rather than moving from nothing, so
      // the first press shows the cursor instead of skipping the first control.
      active_ = true;
      ensureFocus(interactions);
      return true;
    }
    input.focusNext = forward;
    input.focusPrev = !forward;
    interactions.route(input);
    return true;
  }

  /**
   * @brief Confirms the focused chrome control and hands the buttons back.
   *
   * The hand-back is deliberate: a button press is a completed errand, and
   * leaving the chrome holding Confirm afterwards means the next press does
   * something on a bar the player has stopped looking at.
   */
  template <size_t MaxInteractions>
  freeink::ui::ActionEvent confirm(freeink::ui::InteractionBuffer<MaxInteractions>& interactions) {
    freeink::ui::InputSnapshot input;
    input.confirm = true;
    const freeink::ui::ActionEvent event = interactions.route(input);
    active_ = false;
    return event;
  }

 private:
  bool active_ = false;
};

/** Was a direction pressed this pass? Used to take the buttons back from chrome. */
inline bool anyDirectionPressed(const MappedInputManager& mappedInput) {
  return mappedInput.wasPressed(MappedInputManager::Button::Up) ||
         mappedInput.wasPressed(MappedInputManager::Button::Down) ||
         mappedInput.wasPressed(MappedInputManager::Button::Left) ||
         mappedInput.wasPressed(MappedInputManager::Button::Right);
}

/**
 * @brief The focused interaction's rect, or an empty rect when nothing is focused.
 *
 * An app that needs more than "which control" — Solitaire needs to know which
 * *card* in a fanned column, which a tap answers with its y — reads the geometry
 * back out of the buffer here rather than recomputing it. Recomputing hit
 * geometry that something else already computed is the bug class CrossPlay's own
 * Layout struct exists to prevent; the same rule applies to this layer.
 */
template <size_t MaxInteractions>
freeink::ui::Rect focusedRect(const freeink::ui::InteractionBuffer<MaxInteractions>& interactions) {
  const int16_t index = interactions.focusedIndex();
  if (index < 0 || static_cast<size_t>(index) >= interactions.count()) return {};
  return interactions.data()[index].rect;
}

}  // namespace crossplay
