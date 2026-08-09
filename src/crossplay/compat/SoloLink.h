#pragma once

/**
 * @file SoloLink.h
 * @brief PLAY NEARBY's shape, with no radio behind it.
 *
 * Chess and Battleship are written against `linkplay::LinkActivity`, upstream's
 * multiplayer base class. It owns their `loop()` and `render()` (both `final`),
 * draws the searching/disconnect/rematch screens, and runs an ESP-NOW session —
 * about 128KB of source under `link/`, plus `player/` for the device identity
 * that travels in the packets.
 *
 * None of that is ported yet. It is Tier 3, and it is worth less on this device
 * than upstream assumes: PLAY NEARBY needs a second X-series reader within radio
 * range, which most people do not have. The feature actually wanted here is
 * pairing with a stranger over the internet, which upstream has no code for at
 * all.
 *
 * So this file is the same interface with the radio removed. Every hook a game
 * overrides still exists and is still called; `linkPhase()` is permanently
 * `Off`, so `inMatch()` is false, and each game's own solo branch is the only
 * one that runs. The games' sources are unchanged — they already had to work
 * solo, because upstream's own front door is a single-player game with PLAY
 * NEARBY as an option.
 *
 * When multiplayer is built, this file is what gets replaced, and the seam is
 * `linkState()` returning a `PlayBase`: a real transport implements the same
 * five phases and the games do not change again. Upstream's `Transport`
 * interface (`LinkEspNow` / `LinkLoopback` / `FakeLink`) is where an internet
 * transport should slot in.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "../ui/ToyboxScreen.h"
#include "CrossPlayCompat.h"
#include "CrossPlayRect.h"

namespace linkplay {

/** Which game a session is for. Upstream uses it to refuse a mismatched peer. */
enum class GameId : uint8_t { Test = 0, Chess = 1, Battleship = 2, Jaipur = 3 };

/**
 * @brief A match's state machine, permanently switched off.
 *
 * The phases and their meanings are upstream's, kept verbatim so that a real
 * transport dropped in later reports something the games already understand.
 */
class PlayBase {
 public:
  enum class Phase : uint8_t {
    Off,        // not started
    Searching,  // looking for someone
    TheirTurn,  // waiting on them
    YourTurn,   // your move
    Over,       // ending() says why
  };

  enum class Ending : uint8_t {
    None,
    OpponentLeft,
    OpponentLost,
    YouLeft,
  };

  PlayBase() = default;
  ~PlayBase() = default;
  PlayBase(const PlayBase&) = delete;
  PlayBase& operator=(const PlayBase&) = delete;

  /** Always fails: there is no radio to bring up. Games show their own message. */
  bool start(GameId, const char*) { return false; }
  void stop() {}
  Phase update(uint32_t) { return Phase::Off; }

  Phase phase() const { return Phase::Off; }
  Ending ending() const { return Ending::None; }
  /** Only a live match keeps the device awake, and there is never one. */
  bool wantsAwake() const { return false; }
  const char* opponentName() const { return ""; }
};

/**
 * @brief The typed half of upstream's session.
 *
 * The static_asserts are upstream's and are kept even though nothing is sent:
 * they are what stops a game growing a `std::string` into a state that a real
 * transport would later have to copy as bytes. Losing them now would mean
 * discovering the problem when multiplayer is built rather than when the field
 * is added.
 */
template <typename State>
class Play final : public PlayBase {
  static_assert(std::is_trivially_copyable<State>::value,
                "A shared game state is copied as bytes between two devices, so it must be trivially copyable: no "
                "pointers, no std::string, no virtuals.");

 public:
  /** Nothing to send to. Reports failure so a caller's log line is honest. */
  bool play(const State&) { return false; }
  /** Never anything new. `if (takeOpponent(board)) redraw();` stays correct. */
  bool takeOpponent(State&) { return false; }
};

/**
 * @brief What upstream's LinkActivity does when there is no link.
 *
 * `loop()` and `render()` are `final` upstream because tick placement is the
 * thing a game gets wrong, and they stay `final` here for the same reason: a
 * game that starts driving its own pass would be a game that needs editing
 * again when the radio arrives.
 */
class LinkActivity : public CrossPlayActivity {
 public:
  LinkActivity(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : CrossPlayActivity(name, renderer, mappedInput) {}

  void loop() final;
  void render(RenderLock&& lock) final;

  bool preventAutoSleep() override { return linkState().wantsAwake(); }

 protected:
  using Phase = PlayBase::Phase;

  // --- what the game supplies (upstream's contract, unchanged) --------------

  virtual PlayBase& linkState() = 0;
  virtual const PlayBase& linkState() const = 0;
  virtual const char* linkGameTitle() const = 0;
  virtual const char* linkHeadline() const = 0;
  virtual void onMatchStart(bool goesFirst) = 0;
  virtual bool takeOpponentState() = 0;
  virtual void onRematch() = 0;
  virtual void onLinkEnded() = 0;
  virtual bool matchGameOver() const = 0;
  virtual void gameLoop() = 0;
  virtual void gameRender() = 0;

  virtual void onLinkPhaseChanged() {}
  virtual void drawLinkArt(const Rect&) {}

  /**
   * @brief Called when the player asks to play nearby.
   *
   * Upstream starts the radio. Here it records that they asked, so the game can
   * say why nothing happened — see `linkUnavailableReason()`. Deliberately not
   * silent: a button that does nothing reads as a broken build.
   */
  void enterLink(GameId gameId);
  void leaveLink();

  /**
   * @brief Why PLAY NEARBY did nothing, or nullptr if it was never asked for.
   *
   * Consumed and cleared by the caller that shows it.
   */
  const char* takeLinkUnavailableReason();

  bool linkRequested() const { return false; }
  Phase linkPhase() const { return Phase::Off; }
  bool inMatch() const { return false; }
  bool linkYourTurn() const { return false; }
  const char* opponentName() const { return ""; }
  void proposeRematch() {}
  bool linkOwnsScreen() const { return false; }

  /** The visible screen's hit table, filled by the game as it draws. */
  toybox::Interactions interactions;
  /** False between a screen change and its first paint. */
  bool interactionsReady = false;

 private:
  const char* unavailable_ = nullptr;
};

}  // namespace linkplay

namespace player {

/** The longest single word the name lists hold; what a shortName buffer needs. */
constexpr size_t kMaxShortNameLength = 6;

/**
 * @brief What to call somebody in a sentence: their first word.
 *
 * Upstream rolls a three-word device name (SPIKY GRIM BEARD) that doubles as an
 * avatar and travels in the link packets. None of that is ported — there is no
 * link and so no opponent to name — but the two games call this to fill their
 * status lines, so it keeps its contract: copy up to the first space, truncating
 * rather than overrunning, and work on text this build cannot parse.
 */
void shortName(const char* name, char* out, size_t capacity);

}  // namespace player
