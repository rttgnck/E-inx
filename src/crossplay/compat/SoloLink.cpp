#include "SoloLink.h"

#include "CrossPlayFocus.h"

namespace linkplay {

void LinkActivity::loop() {
  // Upstream's loop() is a tick, a phase check, the shared screen's input, and
  // then the game's. With no session, the first three are nothing and this
  // reduces to what every ported app does: paint if asked, then let the game
  // read the buttons.
  pumpRender();
  gameLoop();
}

void LinkActivity::render(RenderLock&&) {
  // Upstream decides here whether the shared link chrome owns the screen or the
  // game does. It never does, so the game always draws — but the hit table is
  // still cleared and re-armed around the build, because the games rely on
  // LinkActivity doing that for them and would otherwise accumulate stale rects
  // until the buffer overflowed.
  interactionsReady = false;
  interactions.clear();
  gameRender();
  crossplay::ensureFocus(interactions);
  interactionsReady = true;
}

void LinkActivity::enterLink(const GameId) {
  // Says so rather than doing nothing. The games route this through their own
  // status line, which is where a player is already looking.
  unavailable_ = "PLAY NEARBY is not in this build";
}

void LinkActivity::leaveLink() { unavailable_ = nullptr; }

const char* LinkActivity::takeLinkUnavailableReason() {
  const char* reason = unavailable_;
  unavailable_ = nullptr;
  return reason;
}

}  // namespace linkplay

namespace player {

void shortName(const char* name, char* out, const size_t capacity) {
  if (!out || capacity == 0) return;
  out[0] = '\0';
  if (!name) return;

  size_t written = 0;
  for (const char* cursor = name; *cursor != '\0' && *cursor != ' '; ++cursor) {
    if (written + 1 >= capacity) break;
    out[written++] = *cursor;
  }
  out[written] = '\0';
}

}  // namespace player
