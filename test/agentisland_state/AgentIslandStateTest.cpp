// Host-side check of agentisland::parseState against payloads shaped like the
// real GET /api/state.
//
// This exists because the first cut of the client buffered the response into a
// 24KB string before parsing it, on the assumption that a snapshot "runs to a
// few KB". It does not: Agent Island's SessionScanner keeps up to 80 activities
// per session, each detail clipped at 6000 characters, across up to 30 sessions.
// Three ordinary sessions already come to about 25KB, so the body was being cut
// off mid-JSON and every poll failed with "unexpected reply" — on a device that
// had otherwise paired, pinned the certificate and been handed a valid token.
//
// The size cases below are therefore the point of the file: the typical one
// straddles the old cap, and the truncation case asserts that a short read is
// reported rather than half-accepted.
//
// Build and run: test/run_agentisland_state.sh
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "agentisland/AgentIslandModel.h"

// Feeds a std::string through the ByteReader interface in small chunks, the way
// the TLS reader does, so the streaming path is what gets exercised.
class StringReader final : public inx::ByteReader {
 public:
  explicit StringReader(std::string text) : text_(std::move(text)) {}
  int read() override {
    char one = 0;
    return readBytes(&one, 1) == 1 ? static_cast<unsigned char>(one) : -1;
  }
  size_t readBytes(char* out, size_t length) override {
    const size_t take = std::min(length, text_.size() - offset_);
    memcpy(out, text_.data() + offset_, take);
    offset_ += take;
    return take;
  }
  size_t consumed() const { return offset_; }
  size_t size() const { return text_.size(); }

 private:
  std::string text_;
  size_t offset_ = 0;
};

static std::string repeated(const char* unit, size_t times) {
  std::string out;
  for (size_t i = 0; i < times; ++i) out += unit;
  return out;
}

static std::string buildPayload(int sessionCount, int activitiesPer, int detailChars) {
  const std::string detail = repeated("abcdefghij ", detailChars / 11);
  std::string json = "{\"version\":3,\"sessions\":[";
  for (int s = 0; s < sessionCount; ++s) {
    if (s > 0) json += ",";
    json += "{\"id\":\"session-" + std::to_string(s) + "\",";
    json += "\"provider\":\"Claude\",\"title\":\"Refactor the parser\",";
    json += "\"project\":\"inx\",\"cwd\":\"/Users/x/inx\",";
    json += s == 0 ? "\"status\":\"needsApproval\"," : "\"status\":\"working\",";
    json += "\"detail\":\"Working\",\"lastMessage\":\"I have finished the change and am running the tests now.\",";
    json += "\"updatedAt\":1754700000.5,\"tokensUsed\":12345,\"activities\":[";
    for (int a = 0; a < activitiesPer; ++a) {
      if (a > 0) json += ",";
      json += "{\"id\":\"act-" + std::to_string(a) + "\",\"kind\":\"assistant\",\"title\":\"Claude\",\"detail\":\"" +
              detail + "\",\"createdAt\":1754700000.5}";
    }
    json += "]";
    if (s == 0) {
      json +=
          ",\"pendingAction\":{\"id\":\"11111111-2222-3333-4444-555555555555\",\"sessionId\":\"session-0\","
          "\"provider\":\"Claude\",\"kind\":\"command\",\"title\":\"Run a command\",\"summary\":\"rm -rf build\","
          "\"detail\":\"" + detail + "\",\"cwd\":\"/Users/x/inx\",\"canAlwaysAllow\":true,\"createdAt\":1754700000.5}";
      json += ",\"pendingActionCount\":2";
    }
    if (s == 1) {
      json +=
          ",\"question\":{\"id\":\"q1\",\"header\":\"Approach\",\"prompt\":\"Which way should I take this?\","
          "\"options\":[{\"label\":\"Streaming\",\"description\":\"Parse as it arrives\"},"
          "{\"label\":\"Buffered\",\"description\":\"Hold it all first\"}],"
          "\"multiSelect\":true,\"allowsOther\":false,\"isSecret\":false}";
    }
    json += "}";
  }
  json += "],\"pendingActions\":[],\"pendingPlans\":[],\"usage\":[],\"messageDelivery\":\"ok\",";
  json += "\"accessibilityTrusted\":true,\"settings\":{\"soundsEnabled\":true}}";
  return json;
}

static bool check(const char* name, int sessions, int activities, int detailChars, int expectAttention) {
  const std::string payload = buildPayload(sessions, activities, detailChars);
  StringReader reader(payload);
  agentisland::Snapshot snapshot;
  std::string error;
  const bool ok = agentisland::parseState(reader, snapshot, &error);

  printf("%-28s payload %7zu B  consumed %7zu  -> %s", name, payload.size(), reader.consumed(),
         ok ? "parsed" : "FAILED");
  if (!ok) {
    printf(" (%s)\n", error.c_str());
    return false;
  }
  printf(", %zu sessions, %d needing attention\n", snapshot.sessions.size(), snapshot.attentionCount());
  if (snapshot.attentionCount() != expectAttention) {
    printf("   !! expected %d needing attention\n", expectAttention);
    return false;
  }
  return true;
}

int main() {
  bool allOk = true;

  allOk &= check("typical (3x20x300)", 3, 20, 300, 2);
  allOk &= check("worst case (30x80x6000)", 30, 80, 6000, 2);
  allOk &= check("old 24KB cap territory", 8, 40, 400, 2);

  // The bug that shipped: a body cut short must be reported, not half-accepted.
  {
    const std::string full = buildPayload(6, 40, 400);
    StringReader truncated(full.substr(0, 24 * 1024));
    agentisland::Snapshot snapshot;
    std::string error;
    const bool ok = agentisland::parseState(truncated, snapshot, &error);
    printf("%-28s truncated at 24576 B          -> %s (%s)\n", "truncation is detected", ok ? "parsed" : "rejected",
           error.c_str());
    allOk &= !ok;
  }

  // A session with a plan, and one with a secret question, take the right paths.
  {
    const std::string json =
        "{\"sessions\":[{\"id\":\"p\",\"provider\":\"Codex\",\"status\":\"needsApproval\",\"title\":\"t\","
        "\"project\":\"inx\",\"detail\":\"d\",\"lastMessage\":\"m\","
        "\"pendingPlan\":{\"id\":\"99999999-8888-7777-6666-555555555555\",\"sessionId\":\"p\",\"provider\":\"Codex\","
        "\"title\":\"The plan\",\"markdown\":\"# Step one\\nDo the thing\",\"cwd\":\"/x\",\"permissions\":[]}},"
        "{\"id\":\"s\",\"provider\":\"Claude\",\"status\":\"needsInput\",\"title\":\"t\",\"project\":\"inx\","
        "\"detail\":\"d\",\"lastMessage\":\"m\",\"question\":{\"id\":\"q\",\"prompt\":\"secret\",\"options\":[],"
        "\"multiSelect\":false,\"allowsOther\":false,\"isSecret\":true}},"
        "{\"id\":\"done\",\"provider\":\"Claude\",\"status\":\"finished\",\"title\":\"t\",\"project\":\"inx\","
        "\"detail\":\"d\",\"lastMessage\":\"m\"}]}";
    StringReader reader(json);
    agentisland::Snapshot snapshot;
    std::string error;
    const bool ok = agentisland::parseState(reader, snapshot, &error);
    printf("%-28s -> %s, %zu sessions kept\n", "plan / secret / finished", ok ? "parsed" : "FAILED",
           snapshot.sessions.size());
    allOk &= ok;
    allOk &= snapshot.sessions.size() == 2;  // the finished one is dropped
    if (ok && snapshot.sessions.size() == 2) {
      const bool planFirst = snapshot.sessions[0].waiting == agentisland::Waiting::Plan;
      const bool secretHandoff = snapshot.sessions[1].waiting == agentisland::Waiting::Handoff;
      printf("   plan card: %s, secret question hands off: %s\n", planFirst ? "yes" : "NO",
             secretHandoff ? "yes" : "NO");
      allOk &= planFirst && secretHandoff;

      printf("   plan command: %s\n",
             agentisland::resolvePlanCommand(snapshot.sessions[0].planId, "acceptAuto").c_str());
    }
  }

  {
    // The compact list: a revision, and rows carrying ids and titles but none of
    // the bodies. The reader has to keep the rev and still build usable rows.
    const std::string json =
        "{\"rev\":\"1741\",\"sessions\":[{\"id\":\"a\",\"provider\":\"Claude\",\"status\":\"needsApproval\","
        "\"title\":\"t\",\"project\":\"inx\",\"detail\":\"d\",\"lastMessage\":\"m\","
        "\"pendingActionCount\":2,\"pendingAction\":{\"id\":\"act-1\",\"kind\":\"bash\",\"title\":\"Run it\"}}]}";
    StringReader reader(json);
    agentisland::Snapshot snapshot;
    std::string error;
    const bool ok = agentisland::parseState(reader, snapshot, &error);
    const bool revKept = snapshot.rev == "1741";
    const bool waits = ok && snapshot.sessions.size() == 1 &&
                       snapshot.sessions[0].waiting == agentisland::Waiting::Action;
    printf("%-28s -> %s, rev=%s, waiting=%s\n", "compact list", ok ? "parsed" : "FAILED", snapshot.rev.c_str(),
           waits ? "action" : "NO");
    allOk &= ok && revKept && waits;
  }

  {
    // The detail fetch merges bodies into a row that already has ids and titles,
    // and must not blank what it does not carry.
    agentisland::Session session;
    session.id = "a";
    session.title = "from the list";
    session.actionId = "act-1";
    session.actionTitle = "Run it";
    const std::string json =
        "{\"id\":\"a\",\"pendingAction\":{\"id\":\"act-1\",\"kind\":\"bash\",\"summary\":\"rm -rf\","
        "\"detail\":\"the whole command\",\"canAlwaysAllow\":true}}";
    StringReader reader(json);
    std::string error;
    const bool ok = agentisland::parseSessionDetail(reader, session, &error);
    const bool merged = session.actionDetail == "the whole command" && session.canAlwaysAllow;
    const bool kept = session.title == "from the list" && session.actionTitle == "Run it";
    printf("%-28s -> %s, detail merged=%s, list fields kept=%s\n", "session detail", ok ? "parsed" : "FAILED",
           merged ? "yes" : "NO", kept ? "yes" : "NO");
    allOk &= ok && merged && kept;
  }

  printf("\n%s\n", allOk ? "ALL OK" : "FAILURES ABOVE");
  return allOk ? 0 : 1;
}
