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

#include "system/Fonts.h"
#include "system/MappedInputManager.h"

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

namespace crossplay {

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

/** Upstream spells the call `GUI.drawButtonHints(renderer, ...)`. */
struct CrossPlayGuiShim {
  void drawButtonHints(const GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                       const char* btn4) const {
    crossplay::drawButtonHints(renderer, btn1, btn2, btn3, btn4);
  }
};

inline constexpr CrossPlayGuiShim GUI{};
