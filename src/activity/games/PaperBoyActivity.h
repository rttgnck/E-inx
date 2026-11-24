#pragma once
#include <SDCardManager.h>
#include <stdint.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

#include "activity/Activity.h"
#include "paperboy/PaperBoyControls.h"

class PaperBoyActivity final : public Activity {
 public:
  PaperBoyActivity(GfxRenderer& r, MappedInputManager& i, std::function<void()> back)
      : Activity("PaperBoyX3", r, i), onBack_(std::move(back)) {}
  ~PaperBoyActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }
  bool allowGlobalPowerRefresh() override { return false; }
  bool handlesPowerButton() override { return true; }

 private:
  enum class Screen { Browser, Playing, Error } screen_ = Screen::Browser;
  std::function<void()> onBack_;
  std::vector<std::string> files_;
  std::string message_, romPath_;
  size_t selected_ = 0;
  bool active_ = false, waitingForIdle_ = false, havePresented_ = false;
  std::array<uint8_t, 5760> presentedLcd_{};
  uint16_t feedback_ = 0, drawnFeedback_ = 0;
  uint8_t loggedKeys_ = 0;
  uint32_t feedbackUntil_ = 0;
  int controlsTop() const;
  void drawControls();
  void drawTab(int x, int y, int width, int height, const char* name, const char* physical, bool active);
  uint32_t nextFrame_ = 0, nextDisplay_ = 0, lastRtc_ = 0, statsAt_ = 0, frames_ = 0, displays_ = 0;
  uint32_t displayUs_ = 0;
  FsFile romFile_;
  PaperBoy::Controls controls_;
  void scan();
  void renderBrowser();
  void renderMessage(const char* title, const char* text);
  void launch();
  bool loadRom();
  bool save();
  void release();
  void drawFrame();
  uint8_t buttons();
  void duringRefresh();
  static PaperBoyActivity* refreshOwner_;
  bool exitPending_ = false, coreFailed_ = false;
  void fail(const std::string& text);
};
