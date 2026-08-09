#include "SolitaireActivity.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstring>

#include "../compat/CrossPlayFocus.h"
#include "../ui/Toybox.h"
#include "../ui/ToyboxFonts.h"
#include "../ui/ToyboxTheme.h"

namespace {

namespace fui = freeink::ui;
namespace ui = solitaireui;

// Upstream writes under /.crosspoint. E-inx keeps its own state under /.einx,
// and putting a game's save anywhere else would mean the reader's own cleanup
// never sees it.
constexpr char kSavePath[] = "/.einx/crossplay/solitaire.sav";
constexpr char kResultsPath[] = "/.einx/crossplay/solitaire.res";
constexpr char kStateDir[] = "/.einx/crossplay";

// A byte a game. A thousand games is a kilobyte, and nobody plays a thousand
// games of solitaire on an e-reader, but the cap means the file cannot grow
// without bound if somebody does.
constexpr int kMaxResults = 2048;

// How many moves may go unwritten.
//
// The save used to go out after every single one. That is about 340 bytes to
// the SD card per press, and a draw-one game runs well past a hundred, so a
// single sitting was a hundred-odd writes — on a card that would rather not
// have them, through a filesystem that has to rewrite a whole block for each.
//
// The board is now written when you leave it, when the app exits, and every
// kSaveEvery moves as a floor against a flat battery. Yank the power mid-game
// and you lose at most the last ten moves, which for a game you can undo
// ninety-six steps of is a fair trade for a tenth of the writes.
constexpr int kSaveEvery = 10;

// Bumped when the save layout changes. An old save is then discarded rather
// than misread, which for a card game means a board with two of the same card
// on it.
constexpr uint8_t kSaveVersion = 1;

}  // namespace

void SolitaireActivity::onEnter() {
  Activity::onEnter();
  // Landscape. This is the whole reason the app looks different from the
  // others: seven columns of cards want a wide screen far more than a page of
  // text wants a tall one.
  renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
  toybox::ensureFonts(renderer);

  hasGame = loadGame();
  if (hasGame) drawThree = game.drawThree();
  view = View::Menu;
  selectedPile = -1;
  recorded = false;
  interactions.setFocusedIndex(-1);
  requestUpdate();
}

void SolitaireActivity::onExit() {
  flushSave();
  // Put the screen back the way it was found. The orientation is global, so
  // leaving it turned would rotate the menu on the way out.
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
}

void SolitaireActivity::newDeal() {
  // millis() is the only entropy on this device that differs between two boots
  // to the same menu. It is a seed, not a key.
  game.deal(static_cast<uint32_t>(millis()), drawThree);
  hasGame = true;
  recorded = false;
  selectedPile = -1;
  view = View::Board;
  // A fresh deal is written immediately rather than debounced: it is the one
  // state where losing the last few moves means losing the whole game.
  unsaved = 0;
  saveGame();
  // A new screen's controls are a new list; starting the ring over means the
  // cursor lands on the stock rather than wherever the menu's third button was.
  interactions.setFocusedIndex(-1);
  requestUpdate();
}

// Called after anything that could have completed the game. Everything a win
// changes happens here, exactly once: the result is recorded, the save is
// dropped so the menu does not offer to resume a finished game, and the view
// moves to the payoff screen with a full refresh behind it.
//
// This deliberately does not live in render(). A screen builder that mutates
// state is a builder that cannot be tested, and render() runs more than once
// per move.
void SolitaireActivity::settleWin() {
  if (!game.won() || recorded) return;
  recorded = true;
  unsaved = 0;
  recordResult(true);
  clearSave();
  hasGame = false;
  selectedPile = -1;
  view = View::Won;
  interactions.setFocusedIndex(-1);
  // The refresh flash as punctuation. A win is the one moment in this game that
  // has earned the full blink.
  flashOnNextPaint = true;
}

/**
 * @brief What Confirm means on a pile, when there is no finger to say where.
 *
 * Upstream reads the tap's y to decide which card of a fanned column you meant.
 * A focus ring has no y — it has a pile and nothing else — so the depth has to
 * come from somewhere, and Klondike genuinely needs it: without multi-card moves
 * a great many deals are unwinnable.
 *
 * The answer is that Confirm on a pile you are already holding digs one card
 * deeper into the run. So:
 *
 *   Confirm on a pile, holding nothing  -> send the top card to a foundation if
 *                                          it can go, else pick that card up
 *   Confirm on the pile you are holding -> take one more card with it, and when
 *                                          there are no more, put the run down
 *   Confirm on a different pile         -> move what you are holding there
 *
 * The first press therefore behaves exactly like upstream's tap, and the extra
 * reach costs nothing until you want it. The selection outline the board already
 * draws is what shows how deep you have dug.
 */
void SolitaireActivity::pickUpOrMove(const int pile) {
  // The stock is a button that happens to look like a pile.
  if (pile == solitaire::kStockPile) {
    selectedPile = -1;
    game.drawFromStock();
    touchSave();
    requestUpdate();
    return;
  }

  const solitaire::Pile& contents = game.pile(pile);

  if (selectedPile < 0) {
    if (contents.empty()) return;
    const int topCard = contents.count - 1;
    // A card that can go straight to a foundation goes, rather than being picked
    // up and making you find the target. This is the move you meant nearly every
    // time, and on a screen this slow a wasted press costs a second.
    if (game.foundationFor(pile) >= 0 && game.runLength(pile, topCard) == 1) {
      const int foundation = game.foundationFor(pile);
      game.move(pile, foundation, 1);
      touchSave();
      settleWin();
      requestUpdate();
      return;
    }
    if (game.runLength(pile, topCard) == 0) return;
    selectedPile = pile;
    selectedCard = topCard;
    requestUpdate();
    return;
  }

  // Same pile again: dig one deeper, or put it down when there is no deeper.
  if (pile == selectedPile) {
    const int deeper = selectedCard - 1;
    if (deeper >= 0 && game.runLength(pile, deeper) > 0) {
      selectedCard = deeper;
    } else {
      selectedPile = -1;
    }
    requestUpdate();
    return;
  }

  const int count = game.pile(selectedPile).count - selectedCard;
  if (game.move(selectedPile, pile, count)) {
    selectedPile = -1;
    touchSave();
    settleWin();
    requestUpdate();
    return;
  }

  // An illegal destination re-aims onto whatever you were pointing at, so a
  // mistake costs one press to recover from instead of two. An empty pile has
  // nothing to re-aim onto, and letting go there was upstream's worst answer:
  // aiming at an empty column while holding a card is unambiguously "put it
  // here", and silently dropping made it look like empty columns did not accept
  // cards at all. They do — a king and nothing else. Keep hold of the card.
  if (!contents.empty()) {
    const int topCard = contents.count - 1;
    if (game.runLength(pile, topCard) > 0) {
      selectedPile = pile;
      selectedCard = topCard;
    } else {
      selectedPile = -1;
    }
  }
  requestUpdate();
}

void SolitaireActivity::routeButton(const int button) {
  switch (button) {
    case ui::ButtonUndo:
      selectedPile = -1;
      if (game.undo()) touchSave();
      requestUpdate();
      break;
    case ui::ButtonNew:
      // Abandoning a game in progress is a result too, and not recording it
      // would let you farm a streak by restarting every deal that went badly.
      if (hasGame && !game.won() && game.moveCount() > 0) recordResult(false);
      newDeal();
      break;
    case ui::ButtonFinish:
      selectedPile = -1;
      game.autoPlay();
      touchSave();
      settleWin();
      requestUpdate();
      break;
    default:
      break;
  }
}

void SolitaireActivity::loop() {
  // E-inx calls loop(), not render(), so the repaint upstream's render task
  // would have run is pumped from here. First, so a press acts on what is on
  // the screen rather than on what is about to be.
  pumpRender();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view == View::Menu) {
      if (!exitTriggered_) {
        exitTriggered_ = true;
        onBack_();
      }
      return;
    }
    if (view == View::Won) {
      view = View::Menu;
    } else {
      flushSave();
      selectedPile = -1;
      view = View::Menu;
    }
    interactions.setFocusedIndex(-1);
    requestUpdate();
    return;
  }

  if (!interactionsReady) return;

  const fui::InputSnapshot input = crossplay::readFocusInput(mappedInput);
  if (!crossplay::hasFocusInput(input)) return;

  const fui::ActionEvent action = interactions.route(input);

  // A focus move dispatches no action but changes what is highlighted, so the
  // screen has to be repainted for the cursor to appear to have moved at all.
  if (action.action == fui::NO_ACTION) {
    requestUpdate();
    return;
  }

  if (action.action == ui::ActionPile && view == View::Board) {
    pickUpOrMove(action.value);
  } else if (action.action == ui::ActionButton) {
    if (view == View::Won) {
      if (action.value == ui::WinAgain) {
        newDeal();
      } else {
        view = View::Menu;
        interactions.setFocusedIndex(-1);
        requestUpdate();
      }
    } else if (view == View::Menu) {
      switch (action.value) {
        case ui::MenuResume:
          view = View::Board;
          interactions.setFocusedIndex(-1);
          requestUpdate();
          break;
        case ui::MenuNew:
          if (hasGame && !game.won() && game.moveCount() > 0) recordResult(false);
          newDeal();
          break;
        case ui::MenuDrawMode:
          // Changes in place and bites on the next deal, so it can sit next to
          // RESUME without threatening the game you have going.
          drawThree = !drawThree;
          requestUpdate();
          break;
        default:
          break;
      }
    } else {
      routeButton(action.value);
    }
  }
}

void SolitaireActivity::render(RenderLock&&) {
  renderer.clearScreen();
  einxui::EInxDrawTarget target = toybox::makeTarget(renderer, toybox::toyboxFaces());
  const fui::InputSnapshot noInput{};
  interactionsReady = false;
  interactions.clear();
  toybox::Frame frame(target, target.deviceContext(), noInput, interactions);

  // A slimmer header than the portrait screens use: in landscape the usual 76
  // would cost a sixth of the height the tableau needs.
  fui::ThemeTokens tokens = toybox::themeTokens();
  tokens.headerHeight = 56;
  toybox::Screen screen(frame, tokens);

  if (view == View::Board) {
    ui::BoardModel model;
    model.game = &game;
    model.selectedPile = selectedPile;
    model.selectedCard = selectedCard;
    model.won = game.won();
    ui::buildBoard(screen, model, layout);
  } else if (view == View::Won) {
    ui::WinModel model;
    model.moves = game.moveCount();
    model.drawThree = game.drawThree();
    ui::MenuModel stats;
    fillStats(stats);
    model.wins = stats.wins;
    model.streak = stats.streak;
    ui::buildWin(screen, model);
  } else {
    ui::MenuModel model;
    model.hasSave = hasGame && !game.won();
    model.savedMoves = game.moveCount();
    model.savedDrawThree = game.drawThree();
    model.drawThree = drawThree;
    fillStats(model);
    ui::buildMenu(screen, model);
  }

  // The controls exist only once the builder has registered them, so the cursor
  // is placed after the build and before the paint that has to show it.
  crossplay::ensureFocus(interactions);
  interactionsReady = true;
  toybox::reportOverflow(interactions, "Solitaire");

  const auto labels = mappedInput.mapLabels("Back", "Select", "Prev", "Next");
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // A win asked for the full blink; every other paint is the fast refresh.
  renderer.displayBuffer(flashOnNextPaint ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  flashOnNextPaint = false;
}

void SolitaireActivity::touchSave() {
  if (++unsaved < kSaveEvery) return;
  flushSave();
}

void SolitaireActivity::flushSave() {
  if (unsaved == 0) return;
  unsaved = 0;
  // A finished game has already had its save removed; writing here would put it
  // back and the menu would offer to resume a game that is over.
  if (!hasGame || game.won()) return;
  saveGame();
}

void SolitaireActivity::saveGame() const {
  solitaire::Game::Save state;
  game.save(state);
  // openFileForWrite will not create the parent, and on a card that has never
  // run CrossPlay there is no parent — so every save would silently fail and
  // RESUME would never appear.
  SdMan.ensureDirectoryExists(kStateDir);
  FsFile file;
  if (!SdMan.openFileForWrite("SOL", kSavePath, file)) return;
  const uint8_t version = kSaveVersion;
  file.write(&version, 1);
  file.write(reinterpret_cast<const uint8_t*>(&state), sizeof(state));
  file.close();
}

bool SolitaireActivity::loadGame() {
  if (!SdMan.exists(kSavePath)) return false;
  FsFile file;
  if (!SdMan.openFileForRead("SOL", kSavePath, file)) return false;
  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != kSaveVersion) {
    file.close();
    return false;
  }
  solitaire::Game::Save state;
  const bool complete = file.read(reinterpret_cast<uint8_t*>(&state), sizeof(state)) == sizeof(state);
  file.close();
  if (!complete) return false;
  // restore() validates the deck, so a truncated or edited save is refused
  // rather than dealt. See SolitaireCore::restore.
  return game.restore(state);
}

void SolitaireActivity::clearSave() const { SdMan.remove(kSavePath); }

void SolitaireActivity::recordResult(const bool won) const {
  auto buffer = makeUniqueNoThrow<uint8_t[]>(kMaxResults);
  if (!buffer) {
    Serial.printf("[SOL] OOM recording result\n");
    return;
  }
  std::memset(buffer.get(), 0, kMaxResults);
  size_t existing = 0;
  FsFile in;
  if (SdMan.openFileForRead("SOL", kResultsPath, in)) {
    existing = in.read(buffer.get(), kMaxResults);
    in.close();
  }
  if (existing >= static_cast<size_t>(kMaxResults)) {
    // Drop the oldest game rather than stop recording. A record that silently
    // stops updating is worse than one with a horizon.
    std::memmove(buffer.get(), buffer.get() + 1, static_cast<size_t>(kMaxResults) - 1);
    existing = static_cast<size_t>(kMaxResults) - 1;
  }
  buffer[existing] = won ? 2 : 1;
  existing++;

  SdMan.ensureDirectoryExists(kStateDir);
  FsFile out;
  if (!SdMan.openFileForWrite("SOL", kResultsPath, out)) return;
  out.write(buffer.get(), existing);
  out.close();
}

void SolitaireActivity::fillStats(ui::MenuModel& model) const {
  auto buffer = makeUniqueNoThrow<uint8_t[]>(kMaxResults);
  if (!buffer) return;
  std::memset(buffer.get(), 0, kMaxResults);
  size_t read = 0;
  FsFile file;
  if (SdMan.openFileForRead("SOL", kResultsPath, file)) {
    read = file.read(buffer.get(), kMaxResults);
    file.close();
  }

  for (size_t i = 0; i < read; ++i) {
    if (buffer[i] == 0) continue;
    model.played++;
    if (buffer[i] == 2) model.wins++;
  }
  for (int i = static_cast<int>(read) - 1; i >= 0; --i) {
    if (buffer[i] != 2) break;
    model.streak++;
  }
  // The last sixteen, oldest first, so the newest game is the bottom-right cell
  // and the grid reads in the same direction as everything else.
  for (int i = 0; i < 16; ++i) {
    const int index = static_cast<int>(read) - 16 + i;
    if (index < 0 || index >= static_cast<int>(read)) continue;
    model.recent[i] = buffer[index];
  }
}
