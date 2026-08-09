#include "DungeonActivity.h"

#include "../compat/CrossPlayFocus.h"
#include "../compat/CrossPlayServices.h"
#include "../compat/CrossPlayCompat.h"

// GUI, for drawButtonHints. Included directly rather than inherited from
// Toybox.h: a header pulled in only for a macro reads as unused to clangd,
// and it was removed once on exactly that reading.
#include "../compat/CrossPlayRect.h"
#include "../ui/Toybox.h"
#include "../ui/ToyboxFonts.h"
#include "../ui/ToyboxTheme.h"

namespace {

namespace fui = freeink::ui;
namespace ui = dungeonui;

constexpr char kSavePath[] = "/.crosspoint/dungeon.sav";

// Bumped whenever the layout below changes, per the cache-format rule. An old
// save is then discarded rather than misread, which here would mean a board
// with walls belonging to a different dungeon.
constexpr uint8_t kSaveVersion = 1;

// How many taps may go unwritten. A dungeon is a few hundred of them and the
// SD card would rather not have a write per tap; the cost of the floor is
// stated rather than hidden: pull the power mid-puzzle and you lose at most
// the last twelve marks.
constexpr int kSaveEvery = 12;

// Everything the save holds. Trivially copyable and written as one block.
struct SaveState {
  uint8_t index;
  uint64_t walls;
  uint64_t floors;
  uint64_t solvedLow;
  uint64_t solvedHigh;
} __attribute__((packed));

}  // namespace

void DungeonActivity::onEnter() {
  toybox::ensureFonts(renderer);
  if (!loadState()) {
    progress = dungeon::Progress{};
    board.load(dungeon::kCampaignFirst);
  }
  // Open on whatever the save was in the middle of, so RESUME means what it
  // says the moment the app appears.
  selected = board.index();
  view = View::Menu;
  recorded = false;
  requestUpdate();
}

void DungeonActivity::onExit() { flushSave(); }

void DungeonActivity::openPuzzle(const int requested) {
  // The tutorial is not a level. Every door into a board funnels through here,
  // so this one guard is the whole of that rule -- a new door cannot get it
  // wrong because there is no choice left at the call site.
  const int index = dungeon::isPlayable(requested) ? requested : dungeon::kCampaignFirst;
  // Reopening the dungeon already in hand keeps its marks; moving to a
  // different one starts clean. Without this, tapping RESUME would wipe the
  // board it offered to resume.
  if (index != board.index()) {
    board.load(index);
    unsaved = 1;
    flushSave();
  }
  recorded = false;
  view = View::Board;
  requestUpdate();
}

// Everything finishing a dungeon changes happens here, exactly once. Not in
// render(): a builder that mutates state cannot be host-tested, and render()
// runs more than once per tap.
void DungeonActivity::settleWin() {
  if (!board.solved() || recorded) return;
  recorded = true;
  lastCleared = board.index();
  progress.markSolved(board.index());
  // The finished board is not worth resuming, so the save carries the next
  // dungeon instead -- and carries the progress, which is the part that matters.
  board.load(progress.nextUnsolved());
  selected = board.index();
  unsaved = 1;
  flushSave();
  view = View::Won;
  // The refresh flash as punctuation, per docs/design-language.md. Finishing a
  // dungeon is the one moment in this game that has earned the full blink.
  flashOnNextPaint = true;
  // And ask for the paint. Without this the winning tap changed the view and
  // drew nothing: the board sat there looking unfinished until the next tap
  // repainted it, which is indistinguishable from the tap having missed.
  requestUpdate();
}

void DungeonActivity::moveCursor() {
  const bool onBoard = view == View::Board;
  const int dx = (mappedInput.wasPressed(MappedInputManager::Button::Right) ? 1 : 0) -
                 (mappedInput.wasPressed(MappedInputManager::Button::Left) ? 1 : 0);
  const int dy = (mappedInput.wasPressed(MappedInputManager::Button::Down) ? 1 : 0) -
                 (mappedInput.wasPressed(MappedInputManager::Button::Up) ? 1 : 0);
  if (dx == 0 && dy == 0) return;

  if (onBoard) {
    const int size = board.size();
    if (size <= 0) return;
    cursorRow = std::clamp(cursorRow + dy, 0, size - 1);
    cursorCol = std::clamp(cursorCol + dx, 0, size - 1);
    requestUpdate();
    return;
  }

  if (view == View::Menu) {
    // The map is a grid, so left/right is one dungeon and up/down is a row of
    // them. Clamped rather than wrapped: the map has a shape and running off the
    // end of it should feel like an edge.
    const int cols = pickerLayout.cols > 0 ? pickerLayout.cols : 8;
    const int first = dungeon::kCampaignFirst;
    const int count = dungeon::kCampaignCount;
    const int offset = std::clamp(selected - first + dx + dy * cols, 0, count - 1);
    if (first + offset == selected) return;
    selected = first + offset;
    requestUpdate();
    return;
  }

  if (view == View::Guide) {
    // The guide is a stack of pages, so the same pad turns them.
    const int step = dx + dy;
    const int page = std::clamp(guidePage + step, 0, ui::guidePageCount() - 1);
    if (page == guidePage) return;
    guidePage = page;
    requestUpdate();
  }
}

void DungeonActivity::routeBoardTap(const int x, const int y) {
  int row = 0;
  int col = 0;
  if (!layout.cellAt(x, y, row, col)) return;
  board.tap(row, col);
  touchSave();
  if (board.solved()) {
    settleWin();
  } else {
    requestUpdate();
  }
}

void DungeonActivity::routeButton(const int button) {
  switch (button) {
    case ui::ButtonPlay:
      openPuzzle(selected);
      break;
    case ui::ButtonGuide:
      guidePage = 0;
      view = View::Guide;
      requestUpdate();
      break;
    case ui::ButtonGuideBack:
      if (guidePage > 0) {
        --guidePage;
      } else {
        view = View::Menu;
      }
      requestUpdate();
      break;
    case ui::ButtonGuideNext:
      // Past the last page is the map you came from, not a dungeon. Dropping
      // the reader straight into a board took the choice away: they had just
      // been taught, and what they want next is to pick, not to be given the
      // next unsolved thing on the list.
      if (guidePage + 1 < ui::guidePageCount()) {
        ++guidePage;
        requestUpdate();
      } else {
        view = View::Menu;
        requestUpdate();
      }
      break;
    case ui::ButtonReset:
      board.reset();
      unsaved = 1;
      flushSave();
      requestUpdate();
      break;
    case ui::ButtonMenu:
      flushSave();
      view = View::Menu;
      requestUpdate();
      break;
    case ui::ButtonNext:
      openPuzzle(progress.nextUnsolved());
      break;
    default:
      break;
  }
}

void DungeonActivity::loop() {
  pumpRender();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (view == View::Menu) {
      if (!exitTriggered_) {
        exitTriggered_ = true;
        onBack_();
      }
      return;
    }
    if (view == View::Board) {
      flushSave();
      view = View::Menu;
      requestUpdate();
    } else if (view == View::Guide && guidePage > 0) {
      --guidePage;
      requestUpdate();
    } else {
      view = View::Menu;
      requestUpdate();
    }
    return;
  }

  if (!interactionsReady) return;

  // The chrome and the play surface share one set of buttons; see
  // compat/CrossPlayFocus.h for the split. A direction always means the board,
  // so pressing one is how the buttons come back from the bar.
  if (crossplay::anyDirectionPressed(mappedInput)) {
    chromeFocus.release();
    moveCursor();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
    chromeFocus.step(interactions, true);
    requestUpdate();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
    chromeFocus.step(interactions, false);
    requestUpdate();
    return;
  }

  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;

  // Confirm on the board rather than the bar: act where the cursor is. The
  // board's cells are not interactions, so this calls the same routing the tap
  // path used, with the cursor standing in for the finger.
  if (!chromeFocus.active()) {
    if (view == View::Board) {
      board.tap(cursorRow, cursorCol);
      touchSave();
      if (board.solved()) {
        settleWin();
      } else {
        requestUpdate();
      }
      return;
    }
    if (view == View::Menu) {
      // On the map, the cursor IS the selection: moving it is looking around and
      // Confirm opens what you are looking at, which is what PLAY does too.
      openPuzzle(selected);
      return;
    }
  }

  const fui::ActionEvent action = chromeFocus.confirm(interactions);
  switch (action.action) {
    case ui::ActionBoard:
      break;
    case ui::ActionButton:
      routeButton(action.value);
      break;
    case ui::ActionPick: {
      // Tapping a cell PICKS it. It used to open it, which made the map sixty-
      // four trapdoors: one stray tap and you were in a dungeon you had not
      // chosen, with the one you were looking at forgotten.
      //
      // The grid registers one target for the whole thing and sends -1, because
      // sixty-four hit rects do not fit the interaction buffer; the layout that
      // drew the cells resolves the tap, so the region cannot drift from the
      // pixels.
      // The grid registers one target for the whole map and sends -1, because
      // sixty-four hit rects do not fit the interaction buffer. Upstream then
      // resolved the tap through the layout; here the map cursor is `selected`
      // already, so there is nothing to resolve.
      const int picked = action.value >= 0 ? action.value : selected;
      if (!dungeon::isPlayable(picked) || picked == selected) break;
      selected = picked;
      requestUpdate();
      break;
    }
    default:
      break;
  }
}

void DungeonActivity::render(RenderLock&&) {
  renderer.clearScreen();
  einxui::EInxDrawTarget target = toybox::makeTarget(renderer, toybox::toyboxFaces());
  const fui::InputSnapshot noInput{};
  interactionsReady = false;
  toybox::Frame frame(target, target.deviceContext(), noInput, interactions);
  toybox::Screen screen(frame);

  switch (view) {
    case View::Board: {
      ui::BoardModel model;
      model.board = &board;
      model.solvedCount = progress.solvedCount();
      model.total = dungeon::kCampaignCount;
      model.solved = board.solved();
      ui::buildBoard(screen, model, layout);
      // The cursor. Upstream has none — a finger needs no pointer — so it is
      // drawn here rather than in the screen builder, which keeps that file
      // diffable against upstream and costs only the geometry `layout` already
      // hands back. Corner marks rather than a fill or an outline: the cell
      // underneath carries the puzzle's own state (filled, crossed, blank), and
      // a cursor that covered it would hide the thing being pointed at.
      if (layout.cell > 0) {
        const int x = layout.board.x + cursorCol * layout.cell;
        const int y = layout.board.y + cursorRow * layout.cell;
        toybox::cornerMarks(renderer, Rect{x, y, layout.cell, layout.cell},
                            layout.cell / 3, toybox::kRule);
      }
      break;
    }
    case View::Guide: {
      ui::GuideModel model;
      model.page = guidePage;
      model.pageCount = ui::guidePageCount();
      ui::buildGuide(screen, model);
      break;
    }
    case View::Won: {
      ui::WinModel model;
      // The dungeon just finished, not the one now loaded: settleWin has
      // already moved the board on to the next one.
      model.dungeonName = dungeon::kPuzzles[lastCleared].name;
      model.cleared = &dungeon::kPuzzles[lastCleared];
      model.solvedCount = progress.solvedCount();
      model.total = dungeon::kCampaignCount;
      model.moreToPlay = progress.solvedCount() < dungeon::kPuzzleCount;
      ui::buildWin(screen, model);
      break;
    }
    case View::Menu:
    default: {
      ui::MenuModel model;
      model.dungeonName = dungeon::kPuzzles[selected].name;
      model.solvedCount = progress.solvedCount();
      model.total = dungeon::kCampaignCount;
      // RESUME only when the picked dungeon IS the one with marks on it.
      // Otherwise the button would offer to resume a board you have not opened.
      model.hasProgress = selected == board.index() && board.touched();
      model.progress = &progress;
      model.selectedIndex = selected;
      ui::buildMenu(screen, model, pickerLayout);
      break;
    }
  }

  interactionsReady = true;
  toybox::reportOverflow(interactions, "Dungeon");

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(flashOnNextPaint ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  flashOnNextPaint = false;
}

void DungeonActivity::touchSave() {
  if (++unsaved < kSaveEvery) return;
  flushSave();
}

void DungeonActivity::flushSave() {
  if (unsaved == 0) return;
  unsaved = 0;
  saveState();
}

void DungeonActivity::saveState() {
  SaveState state;
  state.index = static_cast<uint8_t>(board.index());
  state.walls = board.wallMask();
  state.floors = board.floorMask();
  state.solvedLow = progress.low;
  state.solvedHigh = progress.high;

  HalFile file;
  if (!Storage.openFileForWrite("DUNG", kSavePath, file)) return;
  const uint8_t version = kSaveVersion;
  file.write(&version, 1);
  file.write(reinterpret_cast<const uint8_t*>(&state), sizeof(state));
  file.flush();
}

bool DungeonActivity::loadState() {
  if (!Storage.exists(kSavePath)) return false;
  HalFile file;
  if (!Storage.openFileForRead("DUNG", kSavePath, file)) return false;
  uint8_t version = 0;
  if (file.read(&version, 1) != 1 || version != kSaveVersion) return false;
  SaveState state;
  if (file.read(reinterpret_cast<uint8_t*>(&state), sizeof(state)) != sizeof(state)) return false;
  if (!dungeon::isPlayable(state.index)) {
    // Index 0 lands here too: saves written before the tutorial stopped being a
    // level could name it, and restoring one would put an unplayable board in
    // front of the player with no way to tell why.
    LOG_ERR("DUNG", "Save names dungeon %d, which is not playable", state.index);
    return false;
  }
  progress.low = state.solvedLow;
  progress.high = state.solvedHigh;
  // restore() strips anything standing on a monster or a chest, so an edited
  // or corrupted save costs the position rather than producing a board that
  // cannot be finished. See DungeonCore::restore.
  board.restore(state.index, state.walls, state.floors);
  return true;
}
