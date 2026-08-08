#pragma once

/**
 * @file SolitaireActivity.h
 * @brief Klondike solitaire, in landscape.
 *
 * Ported from CrossPlay's src/apps_local/solitaire/. The rules engine
 * (SolitaireCore) and the screen builders (SolitaireScreens) came across
 * unchanged — they touch no framework — so everything the port cost is in this
 * file and in src/crossplay/compat/.
 *
 * Two changes from upstream are worth knowing about before reading:
 *
 *   1. CrossPlay's Solitaire is played entirely by tapping. The X4 Pro it targets
 *      has a digitiser; the X3 does not, so a faithful port would build, boot,
 *      draw correctly and be unplayable. Input here comes from the buttons,
 *      through FreeInkUI's focus ring. See CrossPlayFocus.h for why that is a
 *      small change rather than a rewrite, and pickUpOrMove() below for the one
 *      genuinely new piece of interaction design it needed.
 *   2. The app is exited through an `onBack` callback, matching E-inx's own
 *      games, rather than upstream's shelf::leave(). E-inx has one live activity
 *      and no stack for an app to pop itself off.
 *
 * This is the only app in the fork that rotates the screen: seven columns of
 * cards want a wide panel far more than a page of text wants a tall one.
 */

#include <functional>
#include <memory>

#include "../compat/CrossPlayCompat.h"
#include "../ui/ToyboxScreen.h"
#include "SolitaireCore.h"
#include "SolitaireScreens.h"

class SolitaireActivity final : public CrossPlayActivity {
 public:
  SolitaireActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onBack)
      : CrossPlayActivity("Solitaire", renderer, mappedInput), onBack_(onBack) {}
  ~SolitaireActivity() override = default;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  /** A card game is a thing you sit and think at; do not sleep out from under it. */
  bool preventAutoSleep() override { return true; }

 private:
  enum class View : uint8_t { Menu, Board, Won };

  void settleWin();
  /** Acts on the focused pile: pick up, dig deeper into a run, or move. */
  void pickUpOrMove(int pile);
  void routeButton(int button);
  void newDeal();

  // The save is the board itself rather than a seed and a move list: replaying
  // moves to rebuild a position is a second implementation of the rules that
  // has to agree with the first one forever.
  void saveGame() const;
  // Records that the board moved on. Writes only every kSaveEvery moves; see
  // the note on that constant for why the every-move version was wrong.
  void touchSave();
  // Writes now if anything is pending. Called on every way out of the board.
  void flushSave();
  bool loadGame();
  void clearSave() const;

  // One byte per finished game, appended: 1 abandoned, 2 won.
  void recordResult(bool won) const;
  void fillStats(solitaireui::MenuModel& model) const;

  const std::function<void()> onBack_;

  solitaire::Game game;
  solitaireui::Layout layout;
  View view = View::Menu;
  // Consumed by the next render(): a full refresh instead of the usual fast
  // one. Only a win sets it.
  bool flashOnNextPaint = false;
  bool hasGame = false;
  bool drawThree = false;
  // Pile -1 means nothing is picked up.
  int selectedPile = -1;
  int selectedCard = 0;
  bool interactionsReady = false;
  // Set on the frame a game is won, so the result is recorded exactly once.
  bool recorded = false;
  // Guards against firing onBack_ twice; the first call frees this object.
  bool exitTriggered_ = false;
  // Moves made since the board was last written to the card.
  int unsaved = 0;
  toybox::Interactions interactions;
};
