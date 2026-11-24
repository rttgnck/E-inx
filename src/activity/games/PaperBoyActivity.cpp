#include "PaperBoyActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "paperboy/PaperBoyCore.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"

namespace {
bool ends(const std::string& s, const char* ext) {
  const size_t n = strlen(ext);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != ext[i]) return false;
  return true;
}
}  // namespace
PaperBoyActivity* PaperBoyActivity::refreshOwner_ = nullptr;
PaperBoyActivity::~PaperBoyActivity() { release(); }
void PaperBoyActivity::onEnter() {
  mappedInput.ignoreInputUntilIdle();
  scan();
  renderBrowser();
}
void PaperBoyActivity::onExit() {
  if (active_ && !save()) Serial.println("[PaperBoyX3] Save failed during activity exit");
  release();
  renderer.requestNextFullRefresh();
}
void PaperBoyActivity::release() {
#ifndef SIMULATOR
  if (refreshOwner_ == this) {
    EInkDisplay::setFrameWaitCallback(nullptr);
    refreshOwner_ = nullptr;
  }
#endif
  pb_stop();
  active_ = false;
  romFile_.close();
}
void PaperBoyActivity::renderMessage(const char* title, const char* text) {
  renderer.clearScreen();
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_16_FONT_ID, 40, title);
  const auto line = renderer.text.truncate(ATKINSON_HYPERLEGIBLE_14_FONT_ID, text, renderer.getScreenWidth() - 32);
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_14_FONT_ID, 100, line.c_str());
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 150, "Back: ROM list");
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
void PaperBoyActivity::fail(const std::string& text) {
  message_ = text;
  screen_ = Screen::Error;
  renderMessage("PaperBoyX3", message_.c_str());
  mappedInput.ignoreInputUntilIdle();
}
void PaperBoyActivity::scan() {
  files_.clear();
  selected_ = 0;
  FsFile dir = SdMan.open("/Games");
  if (!dir || !dir.isDirectory()) {
    message_ = "Create /Games on the SD card";
    return;
  }
  for (FsFile file = dir.openNextFile(); file && files_.size() < 256; file = dir.openNextFile()) {
    char name[256] = {};
    file.getName(name, sizeof(name));
    if (!file.isDirectory() && (ends(name, ".gb") || ends(name, ".gbc") || ends(name, ".gba")))
      files_.emplace_back(name);
    file.close();
  }
  std::sort(files_.begin(), files_.end());
  message_ = files_.empty() ? "Put Game Boy ROMs in /Games" : "GB + DMG-compatible GBC; GBA not yet";
}
void PaperBoyActivity::renderBrowser() {
  renderer.clearScreen();
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_16_FONT_ID, 20, "PaperBoyX3");
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 55, message_.c_str());
  const int rows = std::max(1, (renderer.getScreenHeight() - 260) / 36);
  const size_t first = (selected_ / rows) * rows;
  for (size_t i = first; i < files_.size() && i < first + rows; ++i) {
    std::string label = (i == selected_ ? "> " : "  ") + files_[i];
    label = renderer.text.truncate(ATKINSON_HYPERLEGIBLE_14_FONT_ID, label.c_str(), renderer.getScreenWidth() - 40);
    renderer.text.render(ATKINSON_HYPERLEGIBLE_14_FONT_ID, 20, 95 + (i - first) * 36, label.c_str());
  }
  const int y = renderer.getScreenHeight() - 155;
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, y + 25, "Up/Down: ROM   Confirm: Play   Back: Games");
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, y + 50, "Directions: D-pad   Confirm: A   Back: B");
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, y + 75, "Tap Power: Start   Power+Up: Select");
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, y + 100, "Hold Power+Confirm+Up for 3s: Save & exit");
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

bool PaperBoyActivity::loadRom() {
  romFile_ = SdMan.open(romPath_.c_str());
  if (!romFile_) {
    message_ = "Cannot open SD ROM";
    return false;
  }
  auto read = [](void* context, size_t offset, uint8_t* dest, size_t count) -> bool {
    auto* self = static_cast<PaperBoyActivity*>(context);
    return self->romFile_.seekSet(offset) && self->romFile_.read(dest, count) == static_cast<int>(count);
  };
  if (const char* error = pb_start_reader(read, this, romFile_.size())) {
    message_ = error;
    release();
    return false;
  }
  active_ = true;
  const auto path = romPath_ + ".sav";
  // Recover the previous complete save if power was lost during replacement.
  const auto backup = path + ".bak";
  const auto loadPath = SdMan.exists(path.c_str()) ? path : backup;
  if (SdMan.exists(loadPath.c_str()) && pb_save_size()) {
    FsFile input = SdMan.open(loadPath.c_str());
    if (!input || input.size() != pb_save_size() || input.read(pb_save_data(), pb_save_size()) != pb_save_size()) {
      message_ = "Save size/read error; existing save preserved";
      release();
      return false;
    }
  }
  pb_save_clean();
  return true;
}
void PaperBoyActivity::launch() {
  if (files_.empty()) return;
  if (ends(files_[selected_], ".gba")) {
    fail("GBA needs a separate emulator; not supported");
    return;
  }
  romPath_ = "/Games/" + files_[selected_];
  renderMessage("Loading ROM...", "Reading ROM from SD; please wait");
  if (!loadRom()) {
    fail(message_);
    return;
  }
  controls_ = {};
  waitingForIdle_ = true;
  exitPending_ = coreFailed_ = false;
  havePresented_ = false;
  feedback_ = drawnFeedback_ = 0;
  feedbackUntil_ = 0;
  loggedKeys_ = 0;
  screen_ = Screen::Playing;
  nextFrame_ = micros();
  nextDisplay_ = nextFrame_;
  lastRtc_ = statsAt_ = millis();
  frames_ = displays_ = 0;
  renderer.clearScreen();
  renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, 12, "PaperBoyX3 - detected buttons light up below");
  drawControls();
  renderer.requestNextFullRefresh();
}
bool PaperBoyActivity::save() {
  if (!active_ || !pb_save_size() || !pb_save_dirty()) return true;
  const std::string path = romPath_ + ".sav", temp = path + ".tmp", backup = path + ".bak";
  FsFile output = SdMan.open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!output) return false;
  bool ok = output.write(pb_save_data(), pb_save_size()) == pb_save_size() && output.sync();
  output.close();
  if (!ok) return false;
  if (SdMan.exists(path.c_str())) {
    if (SdMan.exists(backup.c_str()) && !SdMan.remove(backup.c_str())) return false;
    if (!SdMan.rename(path.c_str(), backup.c_str())) return false;
  }
  if (!SdMan.rename(temp.c_str(), path.c_str())) return false;
  pb_save_clean();
  return true;
}
void PaperBoyActivity::drawFrame() {
  const uint8_t* lcd = pb_lcd();
  const bool lcdChanged = !havePresented_ || memcmp(lcd, presentedLcd_.data(), presentedLcd_.size());
  const uint16_t currentFeedback = int32_t(millis() - feedbackUntil_) < 0 ? feedback_ : 0;
  if (!lcdChanged && currentFeedback == drawnFeedback_) return;
  const int top = 48, bottom = controlsTop() - 12;
  int scale = std::max(1, std::min(renderer.getScreenWidth() / 160, (bottom - top) / 144));
  int ox = (renderer.getScreenWidth() - 160 * scale) / 2, oy = top + (bottom - top - 144 * scale) / 2;
  for (int y = 0; y < 144; ++y) {
    if (havePresented_ && !memcmp(lcd + y * 40, presentedLcd_.data() + y * 40, 40)) continue;
    for (int x = 0; x < 160; ++x) {
      uint8_t shade = (lcd[y * 40 + x / 4] >> ((x & 3) * 2)) & 3;
      for (int dy = 0; dy < scale; ++dy)
        for (int dx = 0; dx < scale; ++dx)
          renderer.drawPixel(ox + x * scale + dx, oy + y * scale + dy,
                             PaperBoy::ink(shade, x * scale + dx, y * scale + dy));
    }
  }
  memcpy(presentedLcd_.data(), lcd, presentedLcd_.size());
  havePresented_ = true;
  if (currentFeedback != drawnFeedback_) drawControls();
  uint32_t start = micros();
#ifndef SIMULATOR
  refreshOwner_ = this;
  EInkDisplay::setFrameWaitCallback([] {
    if (refreshOwner_) refreshOwner_->duringRefresh();
  });
#endif
  renderer.displayGameBuffer();
#ifndef SIMULATOR
  EInkDisplay::setFrameWaitCallback(nullptr);
  refreshOwner_ = nullptr;
#endif
  displayUs_ = micros() - start;
  ++displays_;
}
void PaperBoyActivity::loop() {
  using B = MappedInputManager::Button;
  if (screen_ == Screen::Error) {
    if (mappedInput.wasReleased(B::Back)) {
      // Retry failed save before abandoning an active session.
      if (active_ && !save()) {
        fail("Save failed. Check SD; Back retries");
        return;
      }
      release();
      screen_ = Screen::Browser;
      renderBrowser();
      mappedInput.ignoreInputUntilIdle();
    }
    return;
  }
  if (screen_ == Screen::Browser) {
    if (mappedInput.wasReleased(B::Back)) {
      onBack_();
      return;
    }
    bool changed = false;
    if (!files_.empty() && (mappedInput.wasPressed(B::Up) || mappedInput.wasPressed(B::Down))) {
      selected_ = (selected_ + files_.size() + (mappedInput.wasPressed(B::Up) ? -1 : 1)) % files_.size();
      changed = true;
    }
    if (changed) renderBrowser();
    if (mappedInput.wasReleased(B::Confirm)) launch();
    return;
  }
  if (waitingForIdle_) {
    if (mappedInput.isPressed(B::Back) || mappedInput.isPressed(B::Confirm) || mappedInput.isPressed(B::Up) ||
        mappedInput.isPressed(B::Down) || mappedInput.isPressed(B::Left) || mappedInput.isPressed(B::Right) ||
        mappedInput.isPressed(B::Power))
      return;
    waitingForIdle_ = false;
  }
  const uint8_t keys = buttons();
  if (exitPending_ || controls_.exitHeld(keys, millis())) {
    if (!save()) {
      fail("Save failed. Check SD; Back retries");
      return;
    }
    // GamesActivity defers deletion until after this method returns.
    onBack_();
    return;
  }
  if (coreFailed_) {
    fail("SD ROM read or emulator error");
    return;
  }
  // Do not block chord sampling with another full refresh while exiting.
  if ((keys & 12) == 12) return;
  if (uint32_t(millis() - lastRtc_) >= 1000) {
    pb_rtc_tick();
    lastRtc_ += 1000;
  }
  if (int32_t(micros() - nextFrame_) < 0) return;
  bool present = int32_t(micros() - nextDisplay_) >= 0;
  if (!pb_frame(keys, !present)) {
    fail("SD ROM read or emulator error");
    return;
  }
  ++frames_;
  nextFrame_ += 16743;
  // Bound catch-up after a slow panel refresh to keep input responsive.
  if (int32_t(micros() - nextFrame_) > 4 * 16743) nextFrame_ = micros();
  if (present) {
    nextDisplay_ = micros() + 33333;
    drawFrame();
  }
  const uint32_t elapsed = millis() - statsAt_;
  if (elapsed >= 5000) {
    Serial.printf("[PaperBoyX3] emu=%.1f fps panel=%.1f fps last_refresh=%lu us\n", frames_ * 1000.0 / elapsed,
                  displays_ * 1000.0 / elapsed, (unsigned long)displayUs_);
    statsAt_ = millis();
    frames_ = displays_ = 0;
  }
}

uint8_t PaperBoyActivity::buttons() {
  using B = MappedInputManager::Button;
  const uint8_t keys =
      controls_.sample(mappedInput.isPressed(B::Back), mappedInput.isPressed(B::Confirm),
                       mappedInput.isPressed(B::Left), mappedInput.isPressed(B::Right), mappedInput.isPressed(B::Up),
                       mappedInput.isPressed(B::Down), mappedInput.isPressed(B::Power), millis());
  const uint16_t detected = keys | (mappedInput.isPressed(B::Power) ? 256 : 0);
  if (detected) {
    feedback_ = detected;
    feedbackUntil_ = millis() + 500;
  }
  if (keys != loggedKeys_) {
    Serial.printf("[PaperBoyX3] input=%02x\n", keys);
    loggedKeys_ = keys;
  }
  return keys;
}
void PaperBoyActivity::duringRefresh() {
  if (!active_ || coreFailed_ || exitPending_ || int32_t(micros() - nextFrame_) < 0) return;
  // Runs on the same task while the panel is BUSY, outside display SPI transactions.
  // ROM bank reads use their own SD SPI transactions; no display or SD writes occur.
  // CPU/timers advance with rendering skipped, leaving the transmitted image stable.
  mappedInput.update();
  if (uint32_t(millis() - lastRtc_) >= 1000) {
    pb_rtc_tick();
    lastRtc_ += 1000;
  }
  const uint8_t keys = buttons();
  exitPending_ = controls_.exitHeld(keys, millis());
  if ((keys & 12) == 12) return;
  if (!pb_frame(keys, true)) coreFailed_ = true;
  ++frames_;
  nextFrame_ += 16743;
  if (int32_t(micros() - nextFrame_) > 4 * 16743) nextFrame_ = micros();
}

int PaperBoyActivity::controlsTop() const {
  const int line = renderer.text.getLineHeight(ATKINSON_HYPERLEGIBLE_10_FONT_ID);
  return renderer.getScreenHeight() - (2 * line + 12) * 3 - 18 - line;
}
void PaperBoyActivity::drawTab(int x, int y, int width, int height, const char* name, const char* physical,
                               bool active) {
  const int font = ATKINSON_HYPERLEGIBLE_10_FONT_ID;
  renderer.rectangle.fill(x, y, width, height,
                          static_cast<int>(active ? GfxRenderer::FillTone::Ink : GfxRenderer::FillTone::Paper));
  renderer.rectangle.render(x, y, width, height);
  const int line = renderer.text.getLineHeight(font);
  const auto label = renderer.text.truncate(font, name, width - 10, EpdFontFamily::BOLD);
  const auto key = renderer.text.truncate(font, physical, width - 10);
  renderer.text.render(font, x + (width - renderer.text.getWidth(font, label.c_str(), EpdFontFamily::BOLD)) / 2, y + 4,
                       label.c_str(), !active, EpdFontFamily::BOLD);
  renderer.text.render(font, x + (width - renderer.text.getWidth(font, key.c_str())) / 2, y + 4 + line, key.c_str(),
                       !active);
}
void PaperBoyActivity::drawControls() {
  const int font = ATKINSON_HYPERLEGIBLE_10_FONT_ID;
  const int line = renderer.text.getLineHeight(font), stride = 2 * line + 12, h = stride - 6;
  const int sw = renderer.getScreenWidth(), y = controlsTop();
  const uint16_t bits = int32_t(millis() - feedbackUntil_) < 0 ? feedback_ : 0;
  const auto front = mappedInput.mapLabels("B", "A", "LEFT", "RIGHT");
  const char* names[] = {front.btn1, front.btn2, front.btn3, front.btn4};
  int aButton = 2;
  for (int i = 0; i < 4; ++i)
    if (!strcmp(names[i], "A")) aButton = i + 1;
  const bool inverted = mappedInput.invertDirectionalAxes180();
  const int width2 = (sw - 30) / 2, width3 = (sw - 36) / 3, width4 = (sw - 42) / 4;
  drawTab(12, y, width2, h, "START", "Tap Power", bits & 8);
  drawTab(18 + width2, y, width2, h, "SELECT", inverted ? "Power + side lower" : "Power + side upper", bits & 4);
  drawTab(12, y + stride, width3, h, inverted ? "DOWN" : "UP", "Side upper", bits & (inverted ? 128 : 64));
  drawTab(18 + width3, y + stride, width3, h, inverted ? "UP" : "DOWN", "Side lower", bits & (inverted ? 64 : 128));
  drawTab(24 + 2 * width3, y + stride, width3, h, "POWER", "Tap / hold", bits & 256);
  for (int i = 0; i < 4; ++i) {
    char physical[16];
    snprintf(physical, sizeof(physical), "Front %d", i + 1);
    const int mask = !strcmp(names[i], "A") ? 1 : !strcmp(names[i], "B") ? 2 : !strcmp(names[i], "LEFT") ? 32 : 16;
    drawTab(12 + i * (width4 + 6), y + 2 * stride, width4, h, names[i], physical, bits & mask);
  }
  char exit[80];
  snprintf(exit, sizeof(exit), "Exit: Power + Front %d + side %s (3s)", aButton, inverted ? "lower" : "upper");
  renderer.text.centered(font, y + 3 * stride, exit);
  drawnFeedback_ = bits;
}
