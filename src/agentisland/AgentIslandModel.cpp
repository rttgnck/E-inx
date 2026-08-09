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

bool parseState(const std::string& json, Snapshot& out) {
  // Everything outside this filter is discarded during the parse rather than
  // after it: the activity feed alone is bigger than the rest of the payload,
  // and none of it reaches the panel.
  JsonDocument filter;
  JsonObject sessionFilter = filter["sessions"].add<JsonObject>();
  sessionFilter["id"] = true;
  sessionFilter["provider"] = true;
  sessionFilter["title"] = true;
  sessionFilter["project"] = true;
  sessionFilter["status"] = true;
  sessionFilter["detail"] = true;
  sessionFilter["lastMessage"] = true;
  sessionFilter["pendingActionCount"] = true;
  sessionFilter["question"] = true;
  sessionFilter["pendingAction"] = true;
  sessionFilter["pendingPlan"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, json, DeserializationOption::Filter(filter)) != DeserializationError::Ok) {
    return false;
  }
  JsonArrayConst sessions = doc["sessions"].as<JsonArrayConst>();
  if (sessions.isNull()) return false;

  Snapshot snapshot;
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
