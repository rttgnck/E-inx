/**
 * @file BluetoothTransferActivity.cpp
 * @brief Definitions for BluetoothTransferActivity.
 */

#include "BluetoothTransferActivity.h"

#include <GfxRenderer.h>

#include <cstdio>

#include "state/SystemSetting.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/ScreenComponents.h"
#include "system/UiTheme.h"

namespace {

constexpr int CONTENT_MARGIN = 25;
constexpr int LINE_SPACING = 28;
constexpr int SMALL_SPACING = 25;
constexpr int SECTION_SPACING = 40;
constexpr int BOTTOM_AREA_HEIGHT = 80;
constexpr int PROGRESS_BAR_WIDTH = 300;
constexpr int PROGRESS_BAR_HEIGHT = 16;

/** Slowest a progress redraw may happen. A full panel refresh costs several hundred ms. */
constexpr unsigned long PROGRESS_REDRAW_INTERVAL_MS = 3000;

/** Smallest progress step worth spending a refresh on. */
constexpr int PROGRESS_REDRAW_PERCENT_STEP = 10;

int renderActivityHeader(const GfxRenderer& renderer, int startY, const char* title, const char* subtitle = nullptr) {
  return INX_THEME.drawPageHeader(renderer, title, startY, subtitle, CONTENT_MARGIN);
}

std::string truncateString(const std::string& value, size_t maxLength) {
  if (value.length() <= maxLength || maxLength < 4) {
    return value;
  }
  return value.substr(0, maxLength - 3) + "...";
}

std::string formatBytes(uint32_t bytes) {
  char buffer[32];
  if (bytes >= 1024u * 1024u) {
    snprintf(buffer, sizeof(buffer), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    snprintf(buffer, sizeof(buffer), "%.0f KB", static_cast<double>(bytes) / 1024.0);
  }
  return buffer;
}

int percentOf(uint32_t current, uint32_t total) {
  if (total == 0) {
    return 0;
  }
  return static_cast<int>((static_cast<uint64_t>(current) * 100u) / total);
}

}  // namespace

BluetoothTransferActivity::~BluetoothTransferActivity() { server.end(); }

void BluetoothTransferActivity::onEnter() {
  Activity::onEnter();

  startFailed = !server.begin();
  painted = server.status();
  lastPaintAt = millis();

  render();
  SETTINGS.runHalfRefreshOnLoadIfEnabled(renderer, SystemSetting::RefreshOnLoadPage::Sync);
}

void BluetoothTransferActivity::onExit() {
  // Everything the radio owns goes away here, including any .part file still open.
  server.end();
  ActivityWithSubactivity::onExit();
}

bool BluetoothTransferActivity::needsRedraw(const BleTransferStatus& next) const {
  if (next.state != painted.state || next.connected != painted.connected) {
    return true;
  }
  if (next.filename != painted.filename) {
    return true;
  }
  if (next.errorCode != painted.errorCode) {
    return true;
  }
  if (next.lastCompletedAt != painted.lastCompletedAt) {
    return true;
  }

  if (next.state == BleTransferState::Receiving) {
    const int nextPercent = percentOf(next.received, next.total);
    const int paintedPercent = percentOf(painted.received, painted.total);
    const bool steppedEnough = nextPercent - paintedPercent >= PROGRESS_REDRAW_PERCENT_STEP;
    const bool waitedEnough = millis() - lastPaintAt >= PROGRESS_REDRAW_INTERVAL_MS;
    return steppedEnough && waitedEnough;
  }

  return false;
}

void BluetoothTransferActivity::loop() {
  server.poll();

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    exitRequested = true;
  }

  // Confirm doubles as Cancel, but only while there is something to cancel.
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    const BleTransferStatus current = server.status();
    if (current.state == BleTransferState::Receiving) {
      server.cancelTransfer();
    }
  }

  if (exitRequested) {
    if (onComplete) {
      onComplete();
    }
    return;
  }

  const BleTransferStatus next = server.status();
  if (needsRedraw(next)) {
    painted = next;
    lastPaintAt = millis();
    render();
  }
}

void BluetoothTransferActivity::render() const {
  renderer.clearScreen();

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  const char* subtitle = "Waiting for phone...";
  if (startFailed) {
    subtitle = "Unavailable";
  } else if (painted.state == BleTransferState::Receiving) {
    subtitle = "Receiving";
  } else if (painted.state == BleTransferState::Complete) {
    subtitle = "Transfer complete";
  } else if (painted.state == BleTransferState::Failed) {
    subtitle = "Transfer failed";
  } else if (painted.connected) {
    subtitle = "Phone connected";
  }

  const int contentStart = renderActivityHeader(renderer, 0, "Bluetooth Transfer", subtitle);

  if (startFailed) {
    const int centerY = contentStart + (screenHeight - contentStart - BOTTOM_AREA_HEIGHT) / 2;
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY - 20, "Bluetooth could not be started");
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_10_FONT_ID, centerY + 10, "Press Back and try again");

    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  int currentY = contentStart + SECTION_SPACING - 10;

  renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, CONTENT_MARGIN, currentY, "This reader", true,
                       EpdFontFamily::BOLD);
  currentY += LINE_SPACING;
  renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, CONTENT_MARGIN, currentY,
                       BluetoothTransferServer::advertisedName().c_str());
  currentY += LINE_SPACING * 2;

  renderer.line.render(CONTENT_MARGIN, currentY - 10, screenWidth - CONTENT_MARGIN, currentY - 10);
  currentY += SECTION_SPACING;

  if (painted.state == BleTransferState::Receiving) {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, CONTENT_MARGIN, currentY, "Receiving", true,
                         EpdFontFamily::BOLD);
    currentY += LINE_SPACING;

    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, CONTENT_MARGIN, currentY,
                         truncateString(painted.filename, 34).c_str());
    currentY += LINE_SPACING;

    const int barX = (screenWidth - PROGRESS_BAR_WIDTH) / 2;
    ScreenComponents::drawProgressBar(renderer, barX, currentY, PROGRESS_BAR_WIDTH, PROGRESS_BAR_HEIGHT,
                                      painted.received, painted.total);
    currentY += PROGRESS_BAR_HEIGHT + SMALL_SPACING;

    const std::string counts = formatBytes(painted.received) + " / " + formatBytes(painted.total);
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_8_FONT_ID, currentY, counts.c_str());

    const auto labels = mappedInput.mapLabels("« Back", "Cancel", "", "");
    renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (painted.state == BleTransferState::Complete) {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, CONTENT_MARGIN, currentY, "Transfer complete", true,
                         EpdFontFamily::BOLD);
    currentY += LINE_SPACING;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, CONTENT_MARGIN, currentY,
                         truncateString(painted.lastCompletedName, 34).c_str());
    currentY += SMALL_SPACING;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, CONTENT_MARGIN, currentY, "Book added to library");
    currentY += SMALL_SPACING;

    if (painted.lastCompletedMs > 0) {
      const double seconds = static_cast<double>(painted.lastCompletedMs) / 1000.0;
      char rate[64];
      snprintf(rate, sizeof(rate), "%s in %.1f s (%.1f KB/s)", formatBytes(painted.lastCompletedBytes).c_str(), seconds,
               static_cast<double>(painted.lastCompletedBytes) / 1024.0 / (seconds > 0 ? seconds : 1.0));
      renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, CONTENT_MARGIN, currentY, rate);
      currentY += SMALL_SPACING;
    }

    currentY += SMALL_SPACING;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, CONTENT_MARGIN, currentY, "Ready for another book");
  } else if (painted.state == BleTransferState::Failed) {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, CONTENT_MARGIN, currentY, "Transfer failed", true,
                         EpdFontFamily::BOLD);
    currentY += LINE_SPACING;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, CONTENT_MARGIN, currentY,
                         truncateString(painted.errorMessage, 38).c_str());
    currentY += SMALL_SPACING;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, CONTENT_MARGIN, currentY, painted.errorCode.c_str());
    currentY += SMALL_SPACING * 2;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, CONTENT_MARGIN, currentY,
                         "Nothing was added to the library");
    currentY += SMALL_SPACING;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, CONTENT_MARGIN, currentY, "Ready to try again");
  } else {
    renderer.text.render(ATKINSON_HYPERLEGIBLE_12_FONT_ID, CONTENT_MARGIN, currentY,
                         painted.connected ? "Phone connected" : "Waiting for phone...", true, EpdFontFamily::BOLD);
    currentY += LINE_SPACING + 10;

    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, CONTENT_MARGIN, currentY, "1.) Install E-inx Send");
    currentY += SMALL_SPACING;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, CONTENT_MARGIN, currentY, "2.) Choose or share a book");
    currentY += SMALL_SPACING;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_10_FONT_ID, CONTENT_MARGIN, currentY, "3.) Pick this reader and Send");
    currentY += SMALL_SPACING + 20;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, CONTENT_MARGIN, currentY,
                         "Keep this screen open while sending");
    currentY += SMALL_SPACING;
    renderer.text.render(ATKINSON_HYPERLEGIBLE_8_FONT_ID, CONTENT_MARGIN, currentY, "EPUB, TXT, XTC and XTCH");
  }

  const auto labels = mappedInput.mapLabels("« Back", "", "", "");
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
