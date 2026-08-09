#pragma once

/**
 * @file CrossPlayServices.h
 * @brief The firmware services CrossPlay's apps name, spelled E-inx's way.
 *
 * Storage, logging and the button-hint bar. All three exist in both forks with
 * the same shape and different names, so these are aliases rather than
 * implementations — which is the point: an app source that says `Storage.exists`
 * keeps saying it, and stays diffable against upstream.
 */

#include <Arduino.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <WiFi.h>

#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "CrossPlayRect.h"

/**
 * @brief Upstream's storage singleton.
 *
 * A macro rather than a reference because that is what SDCardManager.h already
 * does for `SdMan`, and because `getInstance()` per call sidesteps any
 * static-initialisation order question a namespace-scope reference would raise.
 */
#define Storage SDCardManager::getInstance()

/**
 * @brief Upstream's file handle.
 *
 * `HalFile` and `FsFile` have the same read/write/close/flush surface; upstream's
 * is a thin wrapper over the same SdFat type this firmware uses directly.
 */
using HalFile = FsFile;

// Upstream's logging macros. E-inx logs through Serial directly, so these are
// the same three levels routed there. Kept as macros so the tag-and-format call
// sites in the ported sources are unchanged.
#ifndef LOG_ERR
#define LOG_ERR(tag, fmt, ...) Serial.printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
#ifndef LOG_INF
#define LOG_INF(tag, fmt, ...) Serial.printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
#ifndef LOG_WRN
#define LOG_WRN(tag, fmt, ...) Serial.printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
// Debug is compiled out rather than routed. Its one caller is the chess engine
// reporting node counts on every search, which on a release build is a line of
// serial traffic per move for information nobody is reading.
#ifndef LOG_DBG
#define LOG_DBG(tag, fmt, ...) ((void)0)
#endif

/**
 * @brief Ends a WiFi session, as upstream's `silentRestart()`.
 *
 * Upstream reboots. On CrossPoint that is the supported way to get the network
 * stack back to a clean state after station mode: it sets an RTC_NOINIT flag so
 * setup() skips the splash and routes back to where you were.
 *
 * E-inx does not do that and does not need to. Its own networked screens — the
 * news reader, the library server, the WiFi picker — tear the radio down with
 * `WiFi.disconnect(true)` and `WiFi.mode(WIFI_OFF)` and carry on running. So
 * that is what this does.
 *
 * Rebooting here would also be actively hostile: E-inx has no
 * return-to-where-you-were flag, so a reboot would drop the player out of the
 * app, out of the CrossPlay list, and back to the home screen — every time they
 * closed Hacker News.
 */
inline void silentRestart() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

/** Same thing; upstream's reader-specific variant has no separate meaning here. */
inline void silentRestartToReader() { silentRestart(); }

namespace crossplay {

/**
 * @brief Opens a file with the write cursor at the end, as upstream's
 *        `Storage.openFileForAppend`.
 *
 * E-inx's `openFileForWrite` passes `O_TRUNC`, so it cannot append — and xkcd
 * appends throughout: its archive index and its comic cache both grow an entry
 * at a time, and truncating on each would leave the card holding only the most
 * recent one.
 *
 * `O_AT_END` rather than seeking after the open: an open-then-seek is two
 * operations that can disagree if the file grew between them, and SdFat offers
 * the flag precisely so it does not have to be done by hand.
 *
 * Opened through `FsFile::open`, which resolves against SdFat's current working
 * volume, rather than through SDCardManager. That is not a shortcut: the
 * `SdFat` member is private and SDCardManager lives in the `open-x4-sdk`
 * submodule, so adding the method there would mean this repo recording a
 * submodule commit that does not exist upstream — a checkout nobody else could
 * build. The working volume is set when the card is mounted, so this reaches
 * exactly the same filesystem.
 *
 * Read-modify-write was the other option and is not viable: xkcd appends comic
 * bitmaps to a shared `images.dat`, which grows into the megabytes.
 */
inline bool openFileForAppend(const char* moduleName, const char* path, FsFile& file) {
  if (!file.open(path, O_RDWR | O_CREAT | O_AT_END)) {
    LOG_ERR(moduleName, "Failed to open file for append: %s", path);
    return false;
  }
  return true;
}

/**
 * @brief Streams a URL's body through `onData`, a chunk at a time.
 *
 * CrossPoint's HttpDownloader has an overload taking a DataCallback so a parser
 * can consume a response without buffering it; E-inx's has string and Stream
 * overloads and no streaming one. Connections needs the streaming shape — its
 * puzzle archive is far larger than the free heap — so this supplies it without
 * changing E-inx's own downloader.
 *
 * It lands the body on the card first and then replays it, rather than parsing
 * mid-transfer as upstream does. The memory profile is what matters and it is
 * the same either way (one chunk at a time, never the whole body); the cost is
 * a temporary file and one extra pass over it. Making this a true streaming
 * fetch means adding the overload to E-inx's HttpDownloader, which is a core
 * change and a reasonable follow-up.
 *
 * `onData` returns false to abort, exactly as upstream's does.
 */
bool fetchUrlStreaming(const std::string& url, const std::string& scratchPath,
                       const std::function<bool(const uint8_t*, size_t)>& onData);

/**
 * @brief The four-slot button hint bar, as upstream's `GUI.drawButtonHints`.
 *
 * E-inx draws these through `renderer.ui.buttonHints` at a fixed small cut,
 * which is the same bar the reader and the built-in games use — so a ported app
 * gets the device's own chrome rather than a second style of it.
 */
inline void drawButtonHints(const GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                            const char* btn4) {
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, btn1, btn2, btn3, btn4);
}

}  // namespace crossplay

/** Upstream spells these `GUI.drawButtonHints(renderer, ...)` and `GUI.drawHeader(...)`. */
struct CrossPlayGuiShim {
  void drawButtonHints(const GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const {
    crossplay::drawButtonHints(renderer, btn1, btn2, btn3, btn4);
  }

  /**
   * @brief The header band: a rule under it, and the title if there is one.
   *
   * Upstream fills the band solid and knocks the title out of it. Study passes
   * an empty title and places its own text, so what actually has to be right
   * here is the rule and the band's height — and a solid fill behind text the
   * caller then draws in ink would hide it.
   */
  void drawHeader(const GfxRenderer& renderer, const Rect& band, const char* title) const {
    renderer.line.render(band.x, band.y + band.height - 1, band.x + band.width - 1, band.y + band.height - 1);
    if (title && title[0] != '\0') {
      renderer.text.render(ATKINSON_HYPERLEGIBLE_16_FONT_ID, band.x + 20, band.y + (band.height - 20) / 2, title, true,
                           EpdFontFamily::BOLD);
    }
  }
};

inline constexpr CrossPlayGuiShim GUI{};
