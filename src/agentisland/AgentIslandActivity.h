#pragma once

/**
 * @file AgentIslandActivity.h
 * @brief Public interface and types for AgentIslandActivity.
 *
 * Agent Island on the reader. The Mac app is a notch island with a session
 * feed, usage windows and settings; the Android app mirrors most of that. This
 * is neither. It is the Wear companion's job on a bigger panel: show which
 * sessions are live, what each last said, and answer the thing that is blocking
 * one — and when the answer has to be typed, say so and point at the phone.
 *
 * Two structural notes:
 *
 * - Nothing slow happens on the render path. A fetch is *requested* by setting
 *   `pending_` and asking for a repaint; the loop performs it on the following
 *   pass, once the screen that explains the wait is already on the panel. This
 *   is the same rule HackerNewsActivity follows, and for the same reason: a
 *   blocking call before the repaint is indistinguishable from a hang.
 * - The panel is only pushed when something moved. Polling every few seconds is
 *   what makes approvals arrive; repainting every few seconds is what would
 *   make the screen unreadable and the battery short. The snapshot carries a
 *   signature, and an unchanged signature draws nothing.
 */

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../activity/Activity.h"
#include "agentisland/AgentIslandModel.h"
#include "network/AgentIslandClient.h"
#include "state/AgentIslandPairing.h"

class AgentIslandActivity final : public Activity {
 public:
  AgentIslandActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::function<void()>& onBack)
      : Activity("AgentIsland", renderer, mappedInput), onBack_(onBack) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;

  bool preventAutoSleep() override { return true; }

 private:
  enum class Phase : uint8_t {
    Connecting,  ///< Radio up, endpoint being found, or enrollment in flight.
    NotPaired,   ///< Nothing to connect to: the web manager has the instructions.
    List,        ///< Live sessions.
    Card,        ///< One session's waiting item, with its answers.
    Notice,      ///< A result or a failure; any button returns.
  };

  /** Work the next loop pass should do, once the screen announcing it has been drawn. */
  enum class Work : uint8_t { None, Connect, Refresh, Submit };

  /**
   * How long to wait between snapshots, at minimum. The real interval is paced
   * off how long the last one took — see pollInterval(). A snapshot is not a
   * cheap request on a busy Mac: it carries every session's whole activity
   * feed, which on a heavy day is several megabytes, and fetching that every
   * eight seconds would mean the radio never stops.
   */
  static constexpr uint32_t POLL_INTERVAL_MS = 8000;
  static constexpr uint32_t POLL_INTERVAL_MAX_MS = 120000;
  /** Snapshots past this are worth telling the user about; something is wrong upstream. */
  static constexpr size_t LARGE_SNAPSHOT_BYTES = 1024 * 1024;

  const std::function<void()> onBack_;
  bool exitTriggered_ = false;

  AgentIslandClient client_;
  AgentIslandPairing pairing_;
  agentisland::Snapshot snapshot_;
  uint32_t lastSignature_ = 0;
  unsigned long lastPollMs_ = 0;
  /** Throttles the GPIO re-read inside the abort hook, which is polled per chunk. */
  unsigned long lastAbortPollMs_ = 0;
  uint32_t lastFetchMs_ = 0;
  size_t lastSnapshotBytes_ = 0;

  Phase phase_ = Phase::Connecting;
  Work pending_ = Work::None;
  bool updateRequired_ = false;

  std::string busyMessage_;
  std::string noticeHeadline_;
  std::string noticeBody_;
  std::string connectionLabel_ = "Connecting";
  bool connected_ = false;

  int selectedSession_ = 0;
  int cardChoice_ = 0;
  /** Parallel to the open session's question options, for a multi-select. */
  std::vector<bool> optionChecked_;
  std::string queuedCommand_;
  std::string queuedSuccess_;

  /** Minimum gap before the next snapshot, paced off how long the last one took. */
  uint32_t pollInterval() const;
  /**
   * Installs the abort hook the client polls from inside its blocking loops.
   * Called once on the way in, before any work is queued.
   */
  void installAbortCheck();
  /**
   * True when a blocking call came back because Back was pressed. Leaves the
   * app, since that is what Back does at this level everywhere else.
   */
  bool bailOnCancel(AgentIslandClient::Status status);
  void request(Work what, const char* busyMessage);
  void performConnect();
  void performRefresh();
  void performSubmit();

  void openSelected();
  void activateChoice();
  const agentisland::Session* openSession() const;
  /** Answers offered for the open card, in the order they are drawn. */
  std::vector<std::string> cardChoices() const;

  void showNotice(const char* headline, const std::string& body);
  void showList();

  void render();
  void renderStatusBar() const;
  void renderBusy() const;
  void renderNotPaired() const;
  void renderList() const;
  void renderCard() const;
  void renderNotice() const;

  int contentTop() const;
  int contentBottom() const;
  int rowHeight() const;
  int visibleRows() const;
};
