/**
 * @file IfFoundActivity.cpp
 * @brief Definitions for IfFoundActivity.
 */

#include "IfFoundActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <algorithm>
#include <string>

#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/MenuNav.h"

namespace {
constexpr char kIfFoundPath[] = "/if_found.txt";
constexpr int kTitleFont = ATKINSON_HYPERLEGIBLE_12_FONT_ID;
constexpr int kBodyFont = ATKINSON_HYPERLEGIBLE_10_FONT_ID;
constexpr int kHintFont = ATKINSON_HYPERLEGIBLE_8_FONT_ID;

std::string readIfFoundText() {
  FsFile file;
  if (!SdMan.openFileForRead("IFF", kIfFoundPath, file)) {
    return "";
  }

  std::string out;
  int ch = 0;
  while ((ch = file.read()) >= 0 && out.size() < 4096) {
    if (ch != '\r') {
      out.push_back(static_cast<char>(ch));
    }
  }
  file.close();
  return out;
}
}  // namespace

void IfFoundActivity::onEnter() {
  Activity::onEnter();
  scrollOffset = 0;
  loadText();
  render();
}

void IfFoundActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    onBack();
    return;
  }

  const int pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.text.getLineHeight(kBodyFont) + 5;
  const int visibleLines = std::max(1, (pageHeight - 116) / lineH);
  const int maxScroll = std::max(0, static_cast<int>(lines.size()) - visibleLines);
  if (mappedInput.wasPressed(MenuNav::itemPrev()) && scrollOffset > 0) {
    scrollOffset--;
    render();
  } else if (mappedInput.wasPressed(MenuNav::itemNext()) && scrollOffset < maxScroll) {
    scrollOffset++;
    render();
  }
}

void IfFoundActivity::loadText() {
  std::string text = readIfFoundText();
  if (text.empty()) {
    text =
        "No /if_found.txt file was found on the SD card.\n\n"
        "Create a plain text file named if_found.txt in the SD card root.\n\n"
        "Suggested contents:\n"
        "If found, please contact:\n"
        "Name:\n"
        "Phone or email:\n"
        "Reward or return instructions:";
  }

  lines.clear();
  std::string current;
  const int maxW = renderer.getScreenWidth() - 36;
  const auto flushCurrent = [&]() {
    if (!current.empty()) {
      lines.push_back(current);
      current.clear();
    }
  };

  std::string word;
  for (size_t i = 0; i <= text.size(); ++i) {
    const char c = i < text.size() ? text[i] : '\n';
    if (c == '\n') {
      if (!word.empty()) {
        const std::string candidate = current.empty() ? word : current + " " + word;
        if (!current.empty() && renderer.text.getWidth(kBodyFont, candidate.c_str()) > maxW) {
          flushCurrent();
          current = word;
        } else {
          current = candidate;
        }
        word.clear();
      }
      if (current.empty()) {
        lines.push_back("");
      } else {
        flushCurrent();
      }
      continue;
    }
    if (c == ' ' || c == '\t') {
      if (!word.empty()) {
        const std::string candidate = current.empty() ? word : current + " " + word;
        if (!current.empty() && renderer.text.getWidth(kBodyFont, candidate.c_str()) > maxW) {
          flushCurrent();
          current = word;
        } else {
          current = candidate;
        }
        word.clear();
      }
      continue;
    }
    word.push_back(c);
    if (renderer.text.getWidth(kBodyFont, word.c_str()) > maxW) {
      lines.push_back(renderer.text.truncate(kBodyFont, word.c_str(), maxW));
      word.clear();
    }
  }
  if (lines.empty()) {
    lines.push_back("(empty)");
  }
}

void IfFoundActivity::render() {
  renderer.clearScreen();
  const int pageW = renderer.getScreenWidth();
  const int pageH = renderer.getScreenHeight();

  renderer.text.render(kTitleFont, 18, 18, "If Found", true, EpdFontFamily::BOLD);
  renderer.line.render(0, 52, pageW, 52, true);

  const int lineH = renderer.text.getLineHeight(kBodyFont) + 5;
  const int firstY = 70;
  const int visibleLines = std::max(1, (pageH - 116) / lineH);
  const int maxScroll = std::max(0, static_cast<int>(lines.size()) - visibleLines);
  scrollOffset = std::max(0, std::min(scrollOffset, maxScroll));

  for (int i = 0; i < visibleLines && scrollOffset + i < static_cast<int>(lines.size()); ++i) {
    const std::string& line = lines[static_cast<size_t>(scrollOffset + i)];
    if (!line.empty()) {
      renderer.text.render(kBodyFont, 18, firstY + i * lineH, line.c_str(), true);
    }
  }

  if (maxScroll > 0) {
    char pos[20];
    snprintf(pos, sizeof(pos), "%d/%d", scrollOffset + 1, maxScroll + 1);
    const int w = renderer.text.getWidth(kHintFont, pos);
    renderer.text.render(kHintFont, pageW - w - 18, 56, pos, true);
  }

  const auto labels = mappedInput.mapLabels("\xC2\xAB Back", "Done", "Up", "Down");
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}
