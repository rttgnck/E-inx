/**
 * @file AgentIslandModel.cpp
 * @brief Definitions for AgentIslandModel.
 */

#include "agentisland/AgentIslandModel.h"

#include <ArduinoJson.h>

#include <algorithm>

namespace agentisland {

namespace {

/** Plan markdown arrives whole; the panel shows an excerpt, so the rest is never stored. */
constexpr size_t MAX_PLAN_CHARS = 1200;
constexpr size_t MAX_MESSAGE_CHARS = 400;
constexpr size_t MAX_OPTIONS = 8;

std::string clipped(const char* value, const size_t limit) {
  if (value == nullptr) return "";
  std::string text(value);
  if (text.size() > limit) {
    text.resize(limit);
    text += "…";
  }
  return text;
}

/** Active only. A finished or idle session is history, and history is what the Mac is for. */
bool isInteresting(const std::string& status) {
  return status == "working" || status == "needsApproval" || status == "needsInput" || status == "failed";
}

int attentionRank(const Session& session) {
  if (session.waiting == Waiting::Action) return 0;
  if (session.waiting == Waiting::Question) return 1;
  if (session.waiting == Waiting::Plan) return 2;
  if (session.waiting == Waiting::Handoff) return 3;
  return 4;
}

std::string escapeJson(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char c : value) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escape[8];
          snprintf(escape, sizeof(escape), "\\u%04x", c);
          out += escape;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

uint32_t hashInto(uint32_t seed, const std::string& value) {
  for (const char c : value) {
    seed ^= static_cast<uint8_t>(c);
    seed *= 16777619u;
  }
  return seed;
}

}  // namespace

int Snapshot::attentionCount() const {
  return static_cast<int>(std::count_if(sessions.begin(), sessions.end(),
                                        [](const Session& session) { return session.needsAttention(); }));
}

uint32_t Snapshot::signature() const {
  uint32_t hash = 2166136261u;
  for (const Session& session : sessions) {
    hash = hashInto(hash, session.id);
    hash = hashInto(hash, session.status);
    hash = hashInto(hash, session.actionId);
    hash = hashInto(hash, session.planId);
    hash = hashInto(hash, session.questionPrompt);
    hash = hashInto(hash, session.lastMessage);
    hash ^= static_cast<uint32_t>(session.waiting) + 1u;
    hash *= 16777619u;
  }
  return hash;
}

bool parseState(inx::ByteReader& reader, Snapshot& out, std::string* error) {
  // Everything outside this filter is discarded during the parse rather than
  // after it: the activity feed alone is bigger than the rest of the payload,
  // and none of it reaches the panel. Measured on the host against payloads
  // shaped like the real thing — 24KB in costs 4.4KB of heap, 639KB in costs
  // 23.5KB, and the ceiling SessionScanner permits (30 sessions each with a
  // 6000-character approval detail, 14.7MB in) costs 211KB. Only that last one
  // is beyond this device, it cannot happen in practice, and ArduinoJson
  // reports it as NoMemory rather than failing badly.
  JsonDocument filter;
  filter["rev"] = true;
  JsonObject sessionFilter = filter["sessions"].add<JsonObject>();
  sessionFilter["id"] = true;
  sessionFilter["provider"] = true;
  sessionFilter["title"] = true;
  sessionFilter["project"] = true;
  sessionFilter["status"] = true;
  sessionFilter["detail"] = true;
  sessionFilter["lastMessage"] = true;
  sessionFilter["pendingActionCount"] = true;
  // Named sub-fields rather than `= true` on the whole object: `true` would keep
  // cwd, permissions, timestamps and ids the panel never draws, and each of
  // those is allocated before this file gets a chance to clip anything.
  JsonObject actionFilter = sessionFilter["pendingAction"].to<JsonObject>();
  actionFilter["id"] = true;
  actionFilter["kind"] = true;
  actionFilter["title"] = true;
  actionFilter["summary"] = true;
  actionFilter["detail"] = true;
  actionFilter["canAlwaysAllow"] = true;

  JsonObject planFilter = sessionFilter["pendingPlan"].to<JsonObject>();
  planFilter["id"] = true;
  planFilter["title"] = true;
  planFilter["markdown"] = true;

  JsonObject questionFilter = sessionFilter["question"].to<JsonObject>();
  questionFilter["prompt"] = true;
  questionFilter["header"] = true;
  questionFilter["multiSelect"] = true;
  questionFilter["isSecret"] = true;
  JsonObject optionFilter = questionFilter["options"].add<JsonObject>();
  optionFilter["label"] = true;
  optionFilter["description"] = true;

  JsonDocument doc;
  const DeserializationError failure = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  if (failure != DeserializationError::Ok) {
    if (error) *error = failure.c_str();
    return false;
  }
  JsonArrayConst sessions = doc["sessions"].as<JsonArrayConst>();
  if (sessions.isNull()) {
    if (error) *error = "no session list in the reply";
    return false;
  }

  Snapshot snapshot;
  // A number or a string either way — the reader only echoes it back, so it is
  // kept as text and never interpreted.
  if (doc["rev"].is<const char*>()) {
    snapshot.rev = doc["rev"].as<const char*>();
  } else if (doc["rev"].is<unsigned long long>()) {
    snapshot.rev = std::to_string(doc["rev"].as<unsigned long long>());
  }

  for (JsonObjectConst object : sessions) {
    Session session;
    session.id = object["id"] | "";
    if (session.id.empty()) continue;
    session.status = object["status"] | "idle";

    session.provider = object["provider"] | "Agent";
    session.title = clipped(object["title"] | "", 120);
    session.project = clipped(object["project"] | "", 60);
    session.detail = clipped(object["detail"] | "", 160);
    session.lastMessage = clipped(object["lastMessage"] | "", MAX_MESSAGE_CHARS);

    JsonObjectConst action = object["pendingAction"];
    JsonObjectConst plan = object["pendingPlan"];
    JsonObjectConst question = object["question"];

    if (!action.isNull()) {
      session.waiting = Waiting::Action;
      session.actionId = action["id"] | "";
      session.actionKind = action["kind"] | "permission";
      session.actionTitle = clipped(action["title"] | "", 120);
      session.actionSummary = clipped(action["summary"] | "", 200);
      session.actionDetail = clipped(action["detail"] | "", 600);
      session.canAlwaysAllow = action["canAlwaysAllow"] | false;
      session.actionCount = object["pendingActionCount"] | 1;
      if (session.actionId.empty()) session.waiting = Waiting::Nothing;
    } else if (!plan.isNull()) {
      session.waiting = Waiting::Plan;
      session.planId = plan["id"] | "";
      session.planTitle = clipped(plan["title"] | "", 120);
      session.planMarkdown = clipped(plan["markdown"] | "", MAX_PLAN_CHARS);
      if (session.planId.empty()) session.waiting = Waiting::Nothing;
    } else if (!question.isNull()) {
      session.questionPrompt = clipped(question["prompt"] | "", 400);
      session.questionHeader = clipped(question["header"] | "", 60);
      session.multiSelect = question["multiSelect"] | false;
      for (JsonObjectConst option : question["options"].as<JsonArrayConst>()) {
        if (session.options.size() >= MAX_OPTIONS) break;
        QuestionOption entry;
        entry.label = clipped(option["label"] | "", 90);
        entry.description = clipped(option["description"] | "", 140);
        if (!entry.label.empty()) session.options.push_back(entry);
      }

      const bool secret = question["isSecret"] | false;
      if (secret) {
        session.waiting = Waiting::Handoff;
        session.handoffReason = "This question is protected. Answer it on the Mac.";
      } else if (session.options.empty()) {
        session.waiting = Waiting::Handoff;
        session.handoffReason = "This question wants an answer typed out. Use the phone or Mac app.";
      } else {
        session.waiting = Waiting::Question;
      }
    } else if (session.status == "needsInput") {
      session.waiting = Waiting::Handoff;
      session.handoffReason = "This session is waiting on typed input. Use the phone or Mac app.";
    }

    if (!isInteresting(session.status) && !session.needsAttention()) continue;
    snapshot.sessions.push_back(std::move(session));
  }

  std::stable_sort(snapshot.sessions.begin(), snapshot.sessions.end(),
                   [](const Session& lhs, const Session& rhs) { return attentionRank(lhs) < attentionRank(rhs); });

  snapshot.valid = true;
  out = std::move(snapshot);
  return true;
}

bool parseSessionDetail(inx::ByteReader& reader, Session& session, std::string* error) {
  JsonDocument filter;
  JsonObject actionFilter = filter["pendingAction"].to<JsonObject>();
  actionFilter["id"] = true;
  actionFilter["kind"] = true;
  actionFilter["title"] = true;
  actionFilter["summary"] = true;
  actionFilter["detail"] = true;
  actionFilter["canAlwaysAllow"] = true;

  JsonObject planFilter = filter["pendingPlan"].to<JsonObject>();
  planFilter["id"] = true;
  planFilter["title"] = true;
  planFilter["markdown"] = true;

  JsonObject questionFilter = filter["question"].to<JsonObject>();
  questionFilter["prompt"] = true;
  questionFilter["header"] = true;
  questionFilter["multiSelect"] = true;
  questionFilter["isSecret"] = true;
  JsonObject optionFilter = questionFilter["options"].add<JsonObject>();
  optionFilter["label"] = true;
  optionFilter["description"] = true;

  JsonDocument doc;
  const DeserializationError failure = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  if (failure != DeserializationError::Ok) {
    if (error) *error = failure.c_str();
    return false;
  }

  // Only what the reply carries is written back; the list's ids and titles stay
  // as they were, so a detail response missing a section cannot blank one.
  JsonObjectConst action = doc["pendingAction"].as<JsonObjectConst>();
  if (!action.isNull()) {
    if (action["id"].is<const char*>()) session.actionId = action["id"].as<const char*>();
    if (action["kind"].is<const char*>()) session.actionKind = action["kind"].as<const char*>();
    if (action["title"].is<const char*>()) session.actionTitle = clipped(action["title"].as<const char*>(), 120);
    if (action["summary"].is<const char*>()) session.actionSummary = clipped(action["summary"].as<const char*>(), 200);
    if (action["detail"].is<const char*>()) session.actionDetail = clipped(action["detail"].as<const char*>(), 600);
    session.canAlwaysAllow = action["canAlwaysAllow"] | session.canAlwaysAllow;
  }

  JsonObjectConst plan = doc["pendingPlan"].as<JsonObjectConst>();
  if (!plan.isNull()) {
    if (plan["id"].is<const char*>()) session.planId = plan["id"].as<const char*>();
    if (plan["title"].is<const char*>()) session.planTitle = clipped(plan["title"].as<const char*>(), 120);
    if (plan["markdown"].is<const char*>()) session.planMarkdown = clipped(plan["markdown"].as<const char*>(), MAX_PLAN_CHARS);
  }

  JsonObjectConst question = doc["question"].as<JsonObjectConst>();
  if (!question.isNull()) {
    if (question["prompt"].is<const char*>()) session.questionPrompt = clipped(question["prompt"].as<const char*>(), MAX_MESSAGE_CHARS);
    if (question["header"].is<const char*>()) session.questionHeader = clipped(question["header"].as<const char*>(), 60);
    session.multiSelect = question["multiSelect"] | session.multiSelect;
    JsonArrayConst options = question["options"].as<JsonArrayConst>();
    if (!options.isNull()) {
      session.options.clear();
      for (JsonObjectConst option : options) {
        if (session.options.size() >= MAX_OPTIONS) break;
        QuestionOption entry;
        entry.label = clipped(option["label"] | "", 80);
        entry.description = clipped(option["description"] | "", 160);
        if (!entry.label.empty()) session.options.push_back(entry);
      }
    }
  }
  return true;
}

std::string resolveActionCommand(const std::string& actionId, const bool allow, const bool always) {
  std::string json = "{\"command\":\"resolveAction\",\"id\":\"" + escapeJson(actionId) + "\",\"allow\":";
  json += allow ? "true" : "false";
  json += ",\"always\":";
  json += always ? "true" : "false";
  json += "}";
  return json;
}

std::string resolvePlanCommand(const std::string& planId, const char* decision) {
  return "{\"command\":\"resolvePlan\",\"id\":\"" + escapeJson(planId) + "\",\"decision\":\"" +
         escapeJson(decision) + "\"}";
}

std::string answerQuestionCommand(const std::string& sessionId, const std::vector<std::string>& answers) {
  std::string json = "{\"command\":\"answerQuestion\",\"sessionId\":\"" + escapeJson(sessionId) + "\",\"answers\":[";
  for (size_t i = 0; i < answers.size(); ++i) {
    if (i > 0) json += ",";
    json += "\"" + escapeJson(answers[i]) + "\"";
  }
  json += "]}";
  return json;
}

}  // namespace agentisland
