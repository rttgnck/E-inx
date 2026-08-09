#pragma once

#include <functional>
#include <memory>

#include "../compat/CrossPlayCompat.h"
#include "../compat/CrossPlayFocus.h"
#include "../ui/ToyboxScreen.h"
#include "DungeonCore.h"
#include "DungeonScreens.h"

// D&Diagrams: a nonogram whose clues are a dungeon.
//
// Portrait, like everything except Solitaire. The board is a clue lane plus
// eight 52px cells, which is exactly the width a 480px panel has left after its
// margins -- so the layout is decided by the panel rather than chosen.
class DungeonActivity final : public CrossPlayActivity {
 public:
  DungeonActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                  const std::function<void()>& onBack)
      : CrossPlayActivity("Dungeon", renderer, mappedInput), onBack_(onBack) {}
  ~DungeonActivity() override = default;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class View : uint8_t { Menu, Guide, Board, Won };

  // E-inx has one live activity and no stack, so leaving is a callback the
  // hosting menu supplies rather than upstream's shelf::leave().
  const std::function<void()> onBack_;
  bool exitTriggered_ = false;

  // Where the buttons are pointing. On the board it is a cell; on the map the
  // selection itself is the cursor, so there is no second variable for it.
  crossplay::ChromeFocus chromeFocus;
  int cursorRow = 0;
  int cursorCol = 0;

  // Steps the cursor for whichever direction was pressed, and repaints.
  void moveCursor();

  // Opens a campaign dungeon. The tutorial is not one and is refused here.
  void openPuzzle(int requested);
  void settleWin();
  void routeBoardTap(int x, int y);
  void routeButton(int button);

  // The save is the position itself: an index and two bitmasks. There is no
  // move list, because replaying moves to rebuild a board is a second
  // implementation of the state that has to agree with the first one forever.
  void saveState();
  bool loadState();
  // Marks the board dirty. Written on every way out rather than on every tap:
  // a puzzle is hundreds of taps and the card would rather not have hundreds
  // of writes. See kSaveEvery.
  void touchSave();
  void flushSave();

  dungeon::Board board;
  dungeon::Progress progress;
  dungeonui::Layout layout;
  dungeonui::PickerLayout pickerLayout;
  View view = View::Menu;
  // The dungeon the map has picked out and PLAY would open. Not the same as the
  // one loaded in `board`: you can look around the map without disturbing the
  // game you have in progress, and only PLAY commits.
  int selected = dungeon::kCampaignFirst;
  // Which page of the adventurer's guide is showing. Not saved: the guide is
  // short and always read from the front.
  int guidePage = 0;
  bool interactionsReady = false;
  // Set on the frame a puzzle is finished, so it is recorded exactly once.
  bool recorded = false;
  // The dungeon the win screen is about. Kept because settleWin moves the board
  // on to the next one, and the first version named that next dungeon under the
  // word CLEARED -- a screen congratulating you for a puzzle you have not
  // played.
  int lastCleared = 0;
  // Consumed by the next render(): the full blink instead of the usual fast
  // refresh. Only finishing a dungeon sets it.
  bool flashOnNextPaint = false;
  int unsaved = 0;
  toybox::Interactions interactions;
};
