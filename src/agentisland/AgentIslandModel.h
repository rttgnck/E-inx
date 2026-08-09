#pragma once

/**
 * @file AgentIslandModel.h
 * @brief Public interface and types for AgentIslandModel.
 *
 * The Mac's GET /api/state answers with everything the desktop island knows:
 * every session, its whole activity feed, token counts, usage windows, the
 * settings panel. This device wants a fraction of that — which sessions are
 * live, what each one last said, and what is waiting on an answer — so the
 * snapshot is narrowed on the way in rather than carried around whole.
 *
 * That is the same decision the Wear companion makes, and for the same reason:
 * a watch and an e-ink panel are both places you answer from, not places you
 * read a history in.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace agentisland {

/** What a session is waiting on, and therefore what the card offers. */
enum class Waiting : uint8_t {
  Nothing,   ///< Working, finished or idle — nothing to answer.
  Action,    ///< A tool call or file change wants allow/deny.
  Plan,      ///< A plan is up for review.
  Question,  ///< A choice question with options we can render.
  Handoff,   ///< Needs typing or a secret: the phone or the Mac has to take it.
};

struct QuestionOption {
  std::string label;
  std::string description;
};

struct Session {
  std::string id;
  std::string provider;     ///< "Claude", "Codex", "Cursor", …
  std::string title;
  std::string project;
  std::string status;       ///< working | needsApproval | needsInput | finished | idle | failed
  std::string detail;
  std::string lastMessage;  ///< The last thing the agent said, for context.

  Waiting waiting = Waiting::Nothing;
  std::string handoffReason;  ///< Why this one cannot be answered here.

  // Waiting::Action
  std::string actionId;
  std::string actionTitle;
  std::string actionSummary;
  std::string actionDetail;
  std::string actionKind;
  bool canAlwaysAllow = false;
  int actionCount = 0;  ///< More than one approval queued behind this session.

  // Waiting::Plan
  std::string planId;
  std::string planTitle;
  std::string planMarkdown;

  // Waiting::Question
  std::string questionPrompt;
  std::string questionHeader;
  bool multiSelect = false;
  std::vector<QuestionOption> options;

  bool needsAttention() const { return waiting != Waiting::Nothing; }
};

struct Snapshot {
  std::vector<Session> sessions;
  bool valid = false;

  int attentionCount() const;
  /** A cheap change signal, so the panel is only redrawn when something moved. */
  uint32_t signature() const;
};

/**
 * Parses a GET /api/state body. Returns false and leaves `out` untouched when
 * the body is not the JSON object we expect; a partial parse is treated as a
 * failure, because half a session list is worse than a retry.
 */
bool parseState(const std::string& json, Snapshot& out);

/** Builds the POST /api/command bodies. Kept next to the parser so the wire format lives in one file. */
std::string resolveActionCommand(const std::string& actionId, bool allow, bool always);
std::string resolvePlanCommand(const std::string& planId, const char* decision);
std::string answerQuestionCommand(const std::string& sessionId, const std::vector<std::string>& answers);

}  // namespace agentisland
