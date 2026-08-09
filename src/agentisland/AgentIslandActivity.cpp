/**
 * @file AgentIslandActivity.cpp
 * @brief Definitions for AgentIslandActivity.
 */

#include "agentisland/AgentIslandActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include <algorithm>

#include "state/SystemSetting.h"
#include "system/Fonts.h"
#include "system/MenuNav.h"
#include "system/ScreenComponents.h"
#include "system/UiTheme.h"

using agentisland::Session;
using agentisland::Waiting;

namespace {

constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);
constexpr int kMargin = 20;
constexpr int kRowPad = 10;
constexpr int kChoiceHeight = 46;
constexpr int kChoiceGap = 8;

/**
 * Greedy word wrap. `maxLines` is a hard stop with an ellipsis, because every
 * card on this panel is a fixed box and the alternative to clipping here is
 * clipping in the middle of a glyph.
 */
std::vector<std::string> wrapText(const GfxRenderer& renderer, const int fontId, const std::string& text,
                                  const int maxWidth, const size_t maxLines) {
  std::vector<std::string> lines;
  if (text.empty() || maxLines == 0) return lines;

  std::string line;
  size_t cursor = 0;
  bool truncated = false;

  auto emit = [&](const std::string& value) {
    if (lines.size() < maxLines) {
      lines.push_back(value);
      return true;
    }
    truncated = true;
    return false;
  };

  while (cursor <= text.size()) {
    const size_t space = text.find_first_of(" \n", cursor);
    const std::string word = text.substr(cursor, space == std::string::npos ? std::string::npos : space - cursor);
    const bool newline = space != std::string::npos && text[space] == '\n';

    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && renderer.text.getWidth(fontId, candidate.c_str()) > maxWidth) {
      if (!emit(line)) break;
      line = word;
    } else {
      line = candidate;
    }

    // A single word wider than the box: break it rather than let it run off.
    while (renderer.text.getWidth(fontId, line.c_str()) > maxWidth && line.size() > 1) {
      std::string head = line;
      while (renderer.text.getWidth(fontId, head.c_str()) > maxWidth && head.size() > 1) head.pop_back();
      if (!emit(head)) break;
      line = line.substr(head.size());
    }
    if (truncated) break;

    if (newline) {
      if (!emit(line)) break;
      line.clear();
    }
    if (space == std::string::npos) break;
    cursor = space + 1;
  }

  if (!line.empty() && !truncated) {
    if (lines.size() < maxLines) {
      lines.push_back(line);
    } else {
      truncated = true;
    }
  }
  if (truncated && !lines.empty()) lines.back() += "…";
  return lines;
}

/** The one-word tag on a session row saying what it is waiting for. */
const char* waitingBadge(const Waiting waiting) {
  switch (waiting) {
    case Waiting::Action:
      return "APPROVE";
    case Waiting::Plan:
      return "PLAN";
    case Waiting::Question:
      return "QUESTION";
    case Waiting::Handoff:
      return "ON PHONE";
    case Waiting::Nothing:
    default:
      return "";
  }
}

std::string statusText(const Session& session) {
  if (session.waiting == Waiting::Action) {
    return session.actionCount > 1 ? std::to_string(session.actionCount) + " approvals waiting"
                                   : session.actionSummary;
  }
  if (session.waiting == Waiting::Plan) return "Plan ready for review";
  if (session.waiting == Waiting::Question) return session.questionPrompt;
  if (session.waiting == Waiting::Handoff) return session.handoffReason;
  if (session.status == "working") return session.detail.empty() ? "Working" : session.detail;
  if (session.status == "failed") return session.detail.empty() ? "Failed" : session.detail;
  return session.detail;
}

}  // namespace

void AgentIslandActivity::onEnter() {
  Activity::onEnter();
  pairing_ = AGENT_ISLAND_STORE.get();

  if (!pairing_.hasEndpoint()) {
    phase_ = Phase::NotPaired;
    connectionLabel_ = "Not paired";
    render();
    return;
  }

  request(Work::Connect, "Connecting to Wi-Fi…");
}

void AgentIslandActivity::onExit() {
  // Same courtesy E-inx's other networked screens extend: the radio goes down
  // with the screen, not at the next reboot.
  AgentIslandClient::releaseWifi();
  Activity::onExit();
}

void AgentIslandActivity::request(const Work what, const char* busyMessage) {
  pending_ = what;
  busyMessage_ = busyMessage;
  if (what == Work::Connect || what == Work::Submit) {
    phase_ = Phase::Connecting;
  }
  updateRequired_ = true;
}

void AgentIslandActivity::loop() {
  if (exitTriggered_) return;

  if (updateRequired_) {
    updateRequired_ = false;
    render();
    return;  // The work waits for the next pass, so the panel already shows why.
  }

  if (pending_ != Work::None) {
    const Work work = pending_;
    pending_ = Work::None;
    switch (work) {
      case Work::Connect:
        performConnect();
        break;
      case Work::Refresh:
        performRefresh();
        break;
      case Work::Submit:
        performSubmit();
        break;
      case Work::None:
        break;
    }
    return;
  }

  if ((phase_ == Phase::List || phase_ == Phase::Card) && millis() - lastPollMs_ > pollInterval()) {
    performRefresh();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (phase_ == Phase::Card) {
      showList();
      return;
    }
    exitTriggered_ = true;
    if (onBack_) onBack_();
    return;
  }

  if (phase_ == Phase::Notice || phase_ == Phase::NotPaired) {
    // Confirm returns to the sessions if there are any to return to; a notice
    // raised before the first successful poll has nowhere to go but out.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (phase_ == Phase::Notice && snapshot_.valid) {
        showList();
        return;
      }
      exitTriggered_ = true;
      if (onBack_) onBack_();
    }
    return;
  }

  if (phase_ == Phase::List) {
    const int count = static_cast<int>(snapshot_.sessions.size());
    if (count > 0 && mappedInput.wasPressed(MenuNav::itemPrev())) {
      selectedSession_ = (selectedSession_ - 1 + count) % count;
      updateRequired_ = true;
    }
    if (count > 0 && mappedInput.wasPressed(MenuNav::itemNext())) {
      selectedSession_ = (selectedSession_ + 1) % count;
      updateRequired_ = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      openSelected();
    }
    return;
  }

  if (phase_ == Phase::Card) {
    const int count = static_cast<int>(cardChoices().size());
    if (count > 0 && mappedInput.wasPressed(MenuNav::itemPrev())) {
      cardChoice_ = (cardChoice_ - 1 + count) % count;
      updateRequired_ = true;
    }
    if (count > 0 && mappedInput.wasPressed(MenuNav::itemNext())) {
      cardChoice_ = (cardChoice_ + 1) % count;
      updateRequired_ = true;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateChoice();
    }
  }
}

void AgentIslandActivity::performConnect() {
  const AgentIslandClient::Status wifi = client_.connectWifi();
  if (wifi == AgentIslandClient::Status::NoWifi) {
    connected_ = false;
    connectionLabel_ = "No Wi-Fi";
    showNotice("No Wi-Fi network",
               "Agent Island needs the network the Mac is on. Add one under Sync › Wi-Fi, then come back.");
    return;
  }
  if (wifi == AgentIslandClient::Status::Unsupported) {
    connected_ = false;
    connectionLabel_ = "Unavailable";
    showNotice("No radio here", "Agent Island needs Wi-Fi, which this build does not have.");
    return;
  }

  busyMessage_ = "Looking for " + pairing_.host + "…";
  render();

  unsigned long lastProgressRender = 0;
  const AgentIslandClient::Discovery discovery =
      client_.resolve(pairing_, [this, &lastProgressRender](const int scanned, const int total) {
        // Called once per host. Back is checked every time so the sweep can be
        // abandoned promptly — the main loop is not running to poll GPIO while
        // this blocks, so refresh it here. Repainting is the expensive part, so
        // that still happens only every 50; the panel could not keep up with
        // 253 and does not need to.
        mappedInput.update();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) return false;
        if (scanned - static_cast<int>(lastProgressRender) >= 50) {
          lastProgressRender = static_cast<unsigned long>(scanned);
          busyMessage_ = "Scanning the network… " + std::to_string(scanned) + " of " + std::to_string(total) +
                         "   (Back to stop)";
          render();
        }
        return true;
      });

  if (discovery == AgentIslandClient::Discovery::Cancelled) {
    connected_ = false;
    connectionLabel_ = "Offline";
    showNotice("Stopped looking",
               "The search for " + pairing_.host +
                   " was stopped. Open the app again to retry, or set the Mac's address directly under Settings › "
                   "Agent Island in the web manager.");
    return;
  }

  if (discovery == AgentIslandClient::Discovery::Failed) {
    connected_ = false;
    connectionLabel_ = "Offline";
    showNotice("Can't reach Agent Island",
               "Nothing answered as " + pairing_.host +
                   ", and a sweep of this network found no matching Mac. Check Agent Island is running and that "
                   "both devices are on the same Wi-Fi, then re-pair from the web manager if its address changed.");
    return;
  }

  AGENT_ISLAND_STORE.setResolvedAddress(client_.address());
  pairing_ = AGENT_ISLAND_STORE.get();

  if (pairing_.awaitingEnrollment()) {
    busyMessage_ = "Pairing with Agent Island…";
    render();

    const AgentIslandClient::Result enrolled = client_.enroll(pairing_);
    if (!enrolled.ok()) {
      connected_ = false;
      connectionLabel_ = "Not paired";
      showNotice("Pairing failed", enrolled.status == AgentIslandClient::Status::Unauthorized
                                       ? "That pairing code has expired. Show a new one on the Mac and paste it into "
                                         "the web manager again."
                                       : enrolled.message);
      return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, enrolled.body) != DeserializationError::Ok || !doc["deviceToken"].is<const char*>()) {
      showNotice("Pairing failed", "The Mac accepted the code but did not return a device token.");
      return;
    }
    AGENT_ISLAND_STORE.setDeviceToken(doc["deviceToken"].as<const char*>());
    pairing_ = AGENT_ISLAND_STORE.get();
  }

  if (!pairing_.isPaired()) {
    connected_ = false;
    connectionLabel_ = "Not paired";
    showNotice("Not paired yet",
               "Open Agent Island › Settings on the Mac, copy the pairing link, and paste it into this reader's web "
               "manager under Settings › Agent Island.");
    return;
  }

  performRefresh();
}

uint32_t AgentIslandActivity::pollInterval() const {
  // Leave at least twice the last fetch's duration idle between fetches. A
  // snapshot that takes twenty seconds to arrive polled on an eight-second
  // timer would mean the radio never stops and the panel never settles, which
  // is worse than being a minute behind.
  const uint32_t paced = lastFetchMs_ * 3;
  if (paced <= POLL_INTERVAL_MS) return POLL_INTERVAL_MS;
  return paced > POLL_INTERVAL_MAX_MS ? POLL_INTERVAL_MAX_MS : paced;
}

void AgentIslandActivity::performRefresh() {
  lastPollMs_ = millis();

  // The snapshot is parsed as it streams: it carries every session's whole
  // activity feed, which has no useful upper bound and would not fit here.
  agentisland::Snapshot next;
  const AgentIslandClient::Result result =
      client_.fetchState(pairing_, [&next](inx::ByteReader& reader, std::string& message) {
        std::string reason;
        if (agentisland::parseState(reader, next, &reason)) return true;
        message = "Agent Island's reply could not be read (" + reason + ").";
        return false;
      });

  lastFetchMs_ = result.elapsedMs;
  lastSnapshotBytes_ = result.bytesReceived;
  lastPollMs_ = millis();  // Pace from when the fetch finished, not when it started.
  Serial.printf("[%lu] [AIS] Snapshot %u bytes in %u ms, next poll in %u ms\n", millis(),
                static_cast<unsigned>(result.bytesReceived), static_cast<unsigned>(result.elapsedMs),
                static_cast<unsigned>(pollInterval()));

  if (!result.ok()) {
    connected_ = false;
    connectionLabel_ = "Offline";
    if (result.status == AgentIslandClient::Status::Unauthorized) {
      showNotice("This reader was unpaired",
                 "Agent Island no longer recognises this device. Show a new pairing code on the Mac and paste it into "
                 "the web manager under Settings › Agent Island.");
      return;
    }
    // A dropped poll is not worth throwing the user out of a card they are
    // reading; the status bar says Offline and the next poll tries again. A
    // reply we could not read is different — that will not fix itself, so it
    // gets said out loud with the parser's reason attached.
    if (result.status != AgentIslandClient::Status::BadResponse &&
        (phase_ == Phase::List || phase_ == Phase::Card)) {
      updateRequired_ = true;
      return;
    }
    showNotice(result.status == AgentIslandClient::Status::BadResponse ? "Unexpected reply" : "Lost the connection",
               result.message);
    return;
  }

  connected_ = true;
  connectionLabel_ = client_.address().empty() ? pairing_.host : client_.address();
  // A snapshot this size is not a device problem and the user cannot fix it
  // from here, but it explains why the list is slow to move, so it is said
  // rather than hidden.
  if (lastSnapshotBytes_ >= LARGE_SNAPSHOT_BYTES) {
    char note[32];
    snprintf(note, sizeof(note), " · %.1f MB", static_cast<double>(lastSnapshotBytes_) / (1024.0 * 1024.0));
    connectionLabel_ += note;
  }

  const uint32_t signature = next.signature();
  const std::string openId = phase_ == Phase::Card && selectedSession_ < static_cast<int>(snapshot_.sessions.size())
                                 ? snapshot_.sessions[selectedSession_].id
                                 : "";
  snapshot_ = std::move(next);

  if (phase_ == Phase::Connecting) {
    showList();
    return;
  }

  if (phase_ == Phase::Card) {
    // Keep the card open on the same session across a poll, and fall back to
    // the list if the Mac resolved it in the meantime.
    const auto found = std::find_if(snapshot_.sessions.begin(), snapshot_.sessions.end(),
                                    [&openId](const Session& session) { return session.id == openId; });
    if (found == snapshot_.sessions.end() || !found->needsAttention()) {
      showList();
      return;
    }
    const int index = static_cast<int>(std::distance(snapshot_.sessions.begin(), found));
    if (index != selectedSession_ || signature != lastSignature_) {
      selectedSession_ = index;
      cardChoice_ = std::max(0, std::min(cardChoice_, static_cast<int>(cardChoices().size()) - 1));
      lastSignature_ = signature;
      updateRequired_ = true;
    }
    return;
  }

  if (signature != lastSignature_) {
    lastSignature_ = signature;
    selectedSession_ = std::min(selectedSession_, std::max(0, static_cast<int>(snapshot_.sessions.size()) - 1));
    updateRequired_ = true;
  }
}

void AgentIslandActivity::performSubmit() {
  const AgentIslandClient::Result result = client_.sendCommand(pairing_, queuedCommand_);
  queuedCommand_.clear();

  if (!result.ok()) {
    connected_ = result.status != AgentIslandClient::Status::Unreachable;
    showNotice("Could not send that",
               result.httpStatus == 404
                   ? "It is no longer waiting — someone answered it on another device."
                   : result.message);
    return;
  }

  // The answer landed, so the row is about to disappear. Going straight back to
  // a refreshed list is clearer feedback than a confirmation to dismiss.
  showList();
  performRefresh();
}

void AgentIslandActivity::showNotice(const char* headline, const std::string& body) {
  noticeHeadline_ = headline;
  noticeBody_ = body;
  phase_ = Phase::Notice;
  updateRequired_ = true;
}

void AgentIslandActivity::showList() {
  phase_ = Phase::List;
  cardChoice_ = 0;
  optionChecked_.clear();
  updateRequired_ = true;
}

const Session* AgentIslandActivity::openSession() const {
  if (selectedSession_ < 0 || selectedSession_ >= static_cast<int>(snapshot_.sessions.size())) return nullptr;
  return &snapshot_.sessions[selectedSession_];
}

void AgentIslandActivity::openSelected() {
  const Session* session = openSession();
  if (session == nullptr) return;
  if (!session->needsAttention()) {
    showNotice("Nothing to answer",
               "This session is still working. Its last message is on the list; there is nothing waiting on you yet.");
    return;
  }

  cardChoice_ = 0;
  optionChecked_.assign(session->waiting == Waiting::Question ? session->options.size() : 0, false);
  phase_ = Phase::Card;
  updateRequired_ = true;
}

std::vector<std::string> AgentIslandActivity::cardChoices() const {
  std::vector<std::string> choices;
  const Session* session = openSession();
  if (session == nullptr) return choices;

  switch (session->waiting) {
    case Waiting::Action:
      choices.emplace_back("Allow");
      choices.emplace_back("Deny");
      if (session->canAlwaysAllow) choices.emplace_back("Always allow");
      break;
    case Waiting::Plan:
      choices.emplace_back("Accept");
      choices.emplace_back("Accept & auto-approve");
      choices.emplace_back("Reject");
      choices.emplace_back("Revise — on phone or Mac");
      break;
    case Waiting::Question:
      for (const auto& option : session->options) choices.push_back(option.label);
      if (session->multiSelect) choices.emplace_back("Send answer");
      break;
    case Waiting::Handoff:
    case Waiting::Nothing:
    default:
      choices.emplace_back("Back to sessions");
      break;
  }
  return choices;
}

void AgentIslandActivity::activateChoice() {
  const Session* session = openSession();
  if (session == nullptr) return;
  const std::vector<std::string> choices = cardChoices();
  if (cardChoice_ < 0 || cardChoice_ >= static_cast<int>(choices.size())) return;

  switch (session->waiting) {
    case Waiting::Action: {
      const bool always = choices[cardChoice_] == "Always allow";
      const bool allow = always || cardChoice_ == 0;
      queuedCommand_ = agentisland::resolveActionCommand(session->actionId, allow, always);
      request(Work::Submit, allow ? "Allowing…" : "Denying…");
      return;
    }

    case Waiting::Plan: {
      static const char* kDecisions[] = {"accept", "acceptAuto", "reject"};
      if (cardChoice_ >= 3) {
        showNotice("Revise on the phone or Mac",
                   "Revising a plan means writing what to change, and this reader has no good way to type it. Accept, "
                   "reject, or open the session on the Mac.");
        return;
      }
      queuedCommand_ = agentisland::resolvePlanCommand(session->planId, kDecisions[cardChoice_]);
      request(Work::Submit, "Sending your decision…");
      return;
    }

    case Waiting::Question: {
      if (session->multiSelect) {
        if (cardChoice_ < static_cast<int>(session->options.size())) {
          if (static_cast<size_t>(cardChoice_) < optionChecked_.size()) {
            optionChecked_[cardChoice_] = !optionChecked_[cardChoice_];
          }
          updateRequired_ = true;
          return;
        }
        std::vector<std::string> answers;
        for (size_t i = 0; i < session->options.size() && i < optionChecked_.size(); ++i) {
          if (optionChecked_[i]) answers.push_back(session->options[i].label);
        }
        if (answers.empty()) {
          showNotice("Pick at least one", "This question takes one or more of the options. Select one, then send.");
          return;
        }
        queuedCommand_ = agentisland::answerQuestionCommand(session->id, answers);
        request(Work::Submit, "Sending your answer…");
        return;
      }

      queuedCommand_ = agentisland::answerQuestionCommand(session->id, {session->options[cardChoice_].label});
      request(Work::Submit, "Sending your answer…");
      return;
    }

    case Waiting::Handoff:
    case Waiting::Nothing:
    default:
      showList();
      return;
  }
}

int AgentIslandActivity::contentTop() const { return UiTheme::TOP_STATUS_HEIGHT + 12; }

int AgentIslandActivity::contentBottom() const { return renderer.getScreenHeight() - 62; }

int AgentIslandActivity::rowHeight() const {
  return kRowPad * 2 + renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_12_FONT_ID) +
         renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID) +
         renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_8_FONT_ID) * 2 + 12;
}

void AgentIslandActivity::render() {
  renderer.clearScreen();
  renderStatusBar();

  switch (phase_) {
    case Phase::Connecting:
      renderBusy();
      break;
    case Phase::NotPaired:
      renderNotPaired();
      break;
    case Phase::List:
      renderList();
      break;
    case Phase::Card:
      renderCard();
      break;
    case Phase::Notice:
      renderNotice();
      break;
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void AgentIslandActivity::renderStatusBar() const {
  const int screenWidth = renderer.getScreenWidth();
  const int textY = 10;

  // Connection, on the left: a filled dot when the Mac is answering, hollow when not.
  const int dotY = textY + 4;
  if (connected_) {
    renderer.rectangle.fill(kMargin, dotY, 10, 10, kInk, true);
  } else {
    renderer.rectangle.render(kMargin, dotY, 10, 10, true, true);
  }
  const std::string label =
      renderer.text.truncate(ATKINSON_HYPERLEGIBLE_8_FONT_ID, connectionLabel_.c_str(), screenWidth - 200);
  renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, kMargin + 18, textY, label.c_str());

  // Time and battery, on the right, in the same order and at the same offsets
  // the drawer's own status row uses.
  const int batteryX = screenWidth - 80;
  const std::string time = ScreenComponents::currentTimeText();
  if (!time.empty()) {
    const int width = renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_8_FONT_ID, time.c_str());
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, batteryX - width - 12, textY, time.c_str());
  }
  ScreenComponents::drawBattery(renderer, batteryX, textY,
                                SETTINGS.hideBatteryPercentage != SystemSetting::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);

  renderer.line.render(0, UiTheme::TOP_STATUS_HEIGHT, screenWidth, UiTheme::TOP_STATUS_HEIGHT);
}

void AgentIslandActivity::renderBusy() const {
  const int screenWidth = renderer.getScreenWidth();
  const int centreY = (contentTop() + contentBottom()) / 2;

  renderer.text.centered(ATKINSON_HYPERLEGIBLE_16_FONT_ID, centreY - 40, "Agent Island", true, EpdFontFamily::BOLD);

  const std::vector<std::string> lines =
      wrapText(renderer, ATKINSON_HYPERLEGIBLE_12_FONT_ID, busyMessage_, screenWidth - kMargin * 4, 3);
  const int lineHeight = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_12_FONT_ID) + 4;
  for (size_t i = 0; i < lines.size(); ++i) {
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_12_FONT_ID, centreY + static_cast<int>(i) * lineHeight,
                           lines[i].c_str());
  }

  // No hints: the work below this screen blocks, so a button offered here would
  // not be answered until it had already finished.
}

void AgentIslandActivity::renderNotPaired() const {
  const int screenWidth = renderer.getScreenWidth();
  int y = contentTop() + 20;

  renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, kMargin, y, "Pair with Agent Island", true,
                       EpdFontFamily::BOLD);
  y += renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_16_FONT_ID) + 18;

  static const char* kSteps[] = {
      "1.  On the Mac, open Agent Island › Settings and find the pairing code.",
      "2.  Under the QR code, copy the pairing link — it starts with agentisland://pair.",
      "3.  On this reader, start the web manager: Sync › Library Server.",
      "4.  Open its address in a browser, go to Settings › Agent Island, paste the link and save.",
      "5.  Come back here.",
  };

  const int lineHeight = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID) + 4;
  for (const char* step : kSteps) {
    const std::vector<std::string> lines =
        wrapText(renderer, ATKINSON_HYPERLEGIBLE_10_FONT_ID, step, screenWidth - kMargin * 2 - 12, 3);
    for (const std::string& line : lines) {
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, kMargin, y, line.c_str());
      y += lineHeight;
    }
    y += 10;
  }

  y += 6;
  const std::vector<std::string> why =
      wrapText(renderer, ATKINSON_HYPERLEGIBLE_8_FONT_ID,
               "The link carries the Mac's address and the fingerprint of its certificate. This reader checks that "
               "fingerprint on every connection, so nothing else on the network can answer for it.",
               screenWidth - kMargin * 2, 4);
  for (const std::string& line : why) {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, kMargin, y, line.c_str());
    y += renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_8_FONT_ID) + 2;
  }

  const auto labels = mappedInput.mapLabels("« Apps", "Close", "", "");
  renderer.ui.buttonHintsFit(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

int AgentIslandActivity::visibleRows() const {
  return std::max(1, (contentBottom() - contentTop()) / (rowHeight() + kChoiceGap));
}

void AgentIslandActivity::renderList() const {
  const int screenWidth = renderer.getScreenWidth();

  if (snapshot_.sessions.empty()) {
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, (contentTop() + contentBottom()) / 2 - 20,
                           "No active sessions", true, EpdFontFamily::BOLD);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, (contentTop() + contentBottom()) / 2 + 16,
                           "Nothing is running on the Mac right now.");
    const auto labels = mappedInput.mapLabels("« Apps", "", "", "");
    renderer.ui.buttonHintsFit(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  const int titleHeight = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_12_FONT_ID);
  const int metaHeight = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID);
  const int smallHeight = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_8_FONT_ID);
  const int boxHeight = rowHeight();
  const int rows = visibleRows();

  // Keep the selection on screen without letting the window drift past the end.
  int first = std::max(0, selectedSession_ - rows + 1);
  first = std::min(first, std::max(0, static_cast<int>(snapshot_.sessions.size()) - rows));
  if (selectedSession_ < first) first = selectedSession_;

  for (int i = 0; i < rows; ++i) {
    const int index = first + i;
    if (index >= static_cast<int>(snapshot_.sessions.size())) break;
    const Session& session = snapshot_.sessions[index];
    const bool selected = index == selectedSession_;
    const int rowY = contentTop() + i * (boxHeight + kChoiceGap);
    const int boxWidth = screenWidth - kMargin * 2;

    if (selected) {
      renderer.rectangle.fill(kMargin, rowY, boxWidth, boxHeight, kInk, true);
    } else {
      renderer.rectangle.render(kMargin, rowY, boxWidth, boxHeight, true, true);
    }

    const int textX = kMargin + 14;
    const int textWidth = boxWidth - 28;
    int y = rowY + kRowPad;

    const char* badge = waitingBadge(session.waiting);
    int headerWidth = textWidth;
    if (badge[0] != '\0') {
      const int badgeWidth = renderer.text.getWidth(ATKINSON_HYPERLEGIBLE_8_FONT_ID, badge, EpdFontFamily::BOLD);
      renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, kMargin + boxWidth - 14 - badgeWidth, y + 3, badge,
                           !selected, EpdFontFamily::BOLD);
      headerWidth -= badgeWidth + 16;
    }

    const std::string header =
        session.project.empty() ? session.provider : session.provider + " · " + session.project;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, textX, y,
                         renderer.text.truncate(ATKINSON_HYPERLEGIBLE_12_FONT_ID, header.c_str(), headerWidth).c_str(),
                         !selected, EpdFontFamily::BOLD);
    y += titleHeight + 4;

    renderer.text.render(
        ATKINSON_HYPERLEGIBLE_10_FONT_ID, textX, y,
        renderer.text.truncate(ATKINSON_HYPERLEGIBLE_10_FONT_ID, statusText(session).c_str(), textWidth).c_str(),
        !selected);
    y += metaHeight + 6;

    // The last thing the agent said. Two lines is the whole point of the row:
    // enough to know which session this is without opening it.
    const std::vector<std::string> message =
        wrapText(renderer, ATKINSON_HYPERLEGIBLE_8_FONT_ID, session.lastMessage, textWidth, 2);
    for (const std::string& line : message) {
      renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, textX, y, line.c_str(), !selected);
      y += smallHeight;
    }
  }

  char counter[32];
  snprintf(counter, sizeof(counter), "%d of %d", selectedSession_ + 1, static_cast<int>(snapshot_.sessions.size()));
  const auto labels = mappedInput.mapLabels("« Apps", "Open", "Up", "Down");
  renderer.ui.buttonHintsFit(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, kMargin, contentBottom() + 6, counter);
}

void AgentIslandActivity::renderCard() const {
  const Session* session = openSession();
  if (session == nullptr) return;

  const int screenWidth = renderer.getScreenWidth();
  const int textWidth = screenWidth - kMargin * 2;
  const std::vector<std::string> choices = cardChoices();

  // The answers are anchored to the bottom so their position does not move with
  // the length of whatever is above them.
  const int choicesHeight = static_cast<int>(choices.size()) * (kChoiceHeight + kChoiceGap);
  const int bodyBottom = contentBottom() - choicesHeight - 12;

  int y = contentTop();
  const std::string header = session->project.empty() ? session->provider : session->provider + " · " + session->project;
  renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, kMargin, y,
                       renderer.text.truncate(ATKINSON_HYPERLEGIBLE_8_FONT_ID, header.c_str(), textWidth).c_str());
  y += renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_8_FONT_ID) + 8;

  const char* title = "";
  std::string subtitle;
  std::string body;
  switch (session->waiting) {
    case Waiting::Action:
      title = session->actionTitle.empty() ? "Approval needed" : session->actionTitle.c_str();
      subtitle = session->actionSummary;
      body = session->actionDetail;
      break;
    case Waiting::Plan:
      title = session->planTitle.empty() ? "Plan ready for review" : session->planTitle.c_str();
      body = session->planMarkdown;
      break;
    case Waiting::Question:
      title = session->questionHeader.empty() ? "Question" : session->questionHeader.c_str();
      body = session->questionPrompt;
      break;
    case Waiting::Handoff:
    default:
      title = "Needs the phone or Mac";
      body = session->handoffReason;
      break;
  }

  const std::vector<std::string> titleLines =
      wrapText(renderer, ATKINSON_HYPERLEGIBLE_14_FONT_ID, title, textWidth, 2);
  for (const std::string& line : titleLines) {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, kMargin, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_14_FONT_ID) + 2;
  }
  y += 6;

  if (!subtitle.empty()) {
    const std::vector<std::string> lines =
        wrapText(renderer, ATKINSON_HYPERLEGIBLE_10_FONT_ID, subtitle, textWidth, 3);
    for (const std::string& line : lines) {
      renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, kMargin, y, line.c_str());
      y += renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID) + 2;
    }
    y += 8;
  }

  if (!body.empty()) {
    const int smallHeight = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_8_FONT_ID) + 3;
    const size_t maxLines = static_cast<size_t>(std::max(0, (bodyBottom - y) / smallHeight));
    const std::vector<std::string> lines =
        wrapText(renderer, ATKINSON_HYPERLEGIBLE_8_FONT_ID, body, textWidth, maxLines);
    for (const std::string& line : lines) {
      renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, kMargin, y, line.c_str());
      y += smallHeight;
    }
  }

  int choiceY = contentBottom() - choicesHeight + kChoiceGap;
  for (size_t i = 0; i < choices.size(); ++i) {
    const bool selected = static_cast<int>(i) == cardChoice_;
    const bool checked = session->multiSelect && i < optionChecked_.size() && optionChecked_[i];

    if (selected) {
      renderer.rectangle.fill(kMargin, choiceY, textWidth, kChoiceHeight, kInk, true);
    } else {
      renderer.rectangle.render(kMargin, choiceY, textWidth, kChoiceHeight, true, true);
    }

    int labelX = kMargin + 16;
    if (session->waiting == Waiting::Question && session->multiSelect && i < session->options.size()) {
      const int boxY = choiceY + (kChoiceHeight - 16) / 2;
      renderer.rectangle.render(labelX, boxY, 16, 16, !selected);
      if (checked) renderer.rectangle.fill(labelX + 4, boxY + 4, 8, 8, selected ? 0 : kInk);
      labelX += 26;
    }

    const int labelY = choiceY + (kChoiceHeight - renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_12_FONT_ID)) / 2;
    const int available = kMargin + textWidth - labelX - 14;
    renderer.text.render(
        ATKINSON_HYPERLEGIBLE_12_FONT_ID, labelX, labelY,
        renderer.text.truncate(ATKINSON_HYPERLEGIBLE_12_FONT_ID, choices[i].c_str(), available).c_str(), !selected,
        EpdFontFamily::BOLD);

    choiceY += kChoiceHeight + kChoiceGap;
  }

  const char* confirmLabel = session->waiting == Waiting::Question && session->multiSelect ? "Toggle" : "Choose";
  const auto labels = mappedInput.mapLabels("« Sessions", confirmLabel, "Up", "Down");
  renderer.ui.buttonHintsFit(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void AgentIslandActivity::renderNotice() const {
  const int screenWidth = renderer.getScreenWidth();
  const int textWidth = screenWidth - kMargin * 2;
  int y = contentTop() + 30;

  const std::vector<std::string> headline =
      wrapText(renderer, ATKINSON_HYPERLEGIBLE_16_FONT_ID, noticeHeadline_, textWidth, 2);
  for (const std::string& line : headline) {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, kMargin, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_16_FONT_ID) + 4;
  }
  y += 14;

  const std::vector<std::string> body =
      wrapText(renderer, ATKINSON_HYPERLEGIBLE_10_FONT_ID, noticeBody_, textWidth, 14);
  for (const std::string& line : body) {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, kMargin, y, line.c_str());
    y += renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID) + 4;
  }

  const auto labels = mappedInput.mapLabels("« Apps", "OK", "", "");
  renderer.ui.buttonHintsFit(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
