#pragma once

// Murdle: a murder-mystery deduction game.
//
// The state machine, in full, because getting it wrong is what makes a game
// feel arbitrary. Six views; Back is uniform and has no exceptions -- from Menu
// it leaves the app, from anywhere else it goes to its parent.
//
//                            shelf::leave()
//                                  ^ Back
//                              +---+---+
//                +------------>|  MENU |<--------------+
//                |             +---+---+               |
//           Back |        /        |        \          | DONE
//                |  DIFFICULTY   play      HOW TO      |
//                |        |        |          |        |
//          +-----+--+     |        |     +----+----+   |
//          |SETTINGS|<----+        |     | HOW TO  |   |
//          +--------+              v     +---------+   |
//                             +---------+              |
//                +----------->|  CASE   |              |
//                |            | clues / |              |
//        KEEP    |            |  grid   |              |
//        LOOKING |            +----+----+              |
//                |                 | ACCUSE            |
//                |            +----+-----+             |
//                |            |  ACCUSE  |             |
//                |            +----+-----+             |
//                |                 | CONFIRM           |
//                |            +----+-----+             |
//                +------------+ VERDICT  +-------------+
//                             +----------+
//
// Three things about it are deliberate and are the parts that would otherwise
// be rediscovered as bugs.
//
// CLUES AND GRID ARE NOT TWO VIEWS. They are two faces of one Case state. If
// the toggle were navigation then Back would sometimes mean "the other face"
// and sometimes mean "leave", which stays invisible until somebody loses their
// marks.
//
// EVERY NEW CASE GOES THROUGH ONE FUNNEL. requestNewCase() is the only thing
// that generates, and it is what asks before dropping an open case. A door
// added later cannot get this wrong because there is no choice left at the call
// site. Chess learned this the expensive way: the same defect kept arriving
// through new doors until the intent had one home.
//
// DIFFICULTY NEVER TOUCHES AN OPEN CASE. The setting is read once, at
// generation, and a saved case carries its own shape, so changing the tier
// mid-case is harmless and a restored case comes back the size it was made at.

#include <functional>
#include <memory>

#include "../compat/CrossPlayFocus.h"
#include "../compat/CrossPlayCompat.h"
#include "../ui/ToyboxScreen.h"
#include "MurdleCore.h"
#include "MurdleScreens.h"

class MurdleActivity final : public CrossPlayActivity {
 public:
  MurdleActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onBack)
      : CrossPlayActivity("Murdle", renderer, mappedInput), onBack_(onBack) {}
  ~MurdleActivity() override = default;


  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // E-inx has one live activity and no stack, so the app cannot pop itself: the
  // hosting menu supplies the way out. Upstream's shelf::leave() did the same
  // job through CrossPoint's activity stack.
  const std::function<void()> onBack_;
  bool exitTriggered_ = false;
  // Where the pad is pointing on each of the two play surfaces. Upstream needs
  // neither: a finger points at what it means.
  int gridRow = 0;
  int gridCol = 0;
  int clueCursor = 0;
  crossplay::ChromeFocus chromeFocus;
  void moveGridCursor();
  void moveClueCursor();

  enum class View : uint8_t {
    Menu,
    Case,
    Accuse,
    Verdict,
    Settings,
    HowTo,
    ConfirmNew,
  };

  void goToMenu();
  void openCase();
  // The one door to a new case. Asks when an unsolved one is open, generates
  // otherwise. Nothing else may call generateCase().
  void requestNewCase();
  void generateCase();
  void routeAction(int action, int value);
  void handleGridTap(int x, int y);
  void handleClueTap(int y);
  void submitAccusation();

  void saveState();
  bool loadState();
  void flushSave();
  void recordVerdict(int outcome);

  murdle::Puzzle puzzle;
  murdle::Marks marks;
  // ~4.7KB of working room for the generator, held only while a case is being
  // made. Too big for the task stack, and not worth keeping in DRAM between
  // cases on a device with 380KB of it.
  std::unique_ptr<murdle::Scratch> scratch;

  View view = View::Menu;
  murdleui::Face face = murdleui::Face::Clues;
  murdleui::GridLayout gridLayout;

  bool hasCase = false;
  bool solved = false;
  murdle::Tier tier = murdle::Tier::Elementary;
  uint32_t seed = 0;
  int caseNumber = 0;
  int page = 0;
  uint32_t struck = 0;
  int wrongAccusations = 0;

  int solvedCount = 0;
  int wrongCount = 0;
  uint32_t record = 0;

  uint8_t picks[murdle::kMaxCats] = {};
  int howToPage = 0;

  // Generation takes long enough to be worth a frame of its own, so it is
  // deferred by one loop pass: the "DEDUCING" paint lands before the work
  // starts rather than after it.
  bool generatePending = false;

  bool dirty = false;
  bool flashOnNextPaint = false;
  bool interactionsReady = false;
  toybox::Interactions interactions;
};
