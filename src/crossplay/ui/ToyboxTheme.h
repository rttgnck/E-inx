#pragma once

/**
 * @file ToyboxTheme.h
 * @brief The renderer-side half of the Toybox theme.
 *
 * Binds real E-inx fonts to FreeInkUI's three font slots, and notices an
 * overflowing interaction table. The tokens themselves are freestanding, in
 * ToyboxTokens.h, which is what keeps screen builders host-testable.
 *
 * Ported from CrossPlay's ui/ToyboxTheme.h. The only structural change: upstream
 * includes <FreeInkUIGfxRenderer.h> and builds a `fui::GfxRendererTarget`; that
 * SDK adapter binds to CrossPoint's flat GfxRenderer and was dropped on
 * vendoring, so this builds an einxui::EInxDrawTarget instead.
 */

#include <Arduino.h>

#include "../compat/EInxDrawTarget.h"
#include "ToyboxFonts.h"
#include "ToyboxScreen.h"
#include "ToyboxTokens.h"

namespace toybox {

/**
 * @brief Which typefaces a screen speaks in.
 *
 * Toybox is the shapes — the black header band, the pill buttons, the hairline
 * rows, the proportions — and those stay shared so the apps read as one system.
 * The faces do not: a game gets its own, so you can tell what you are playing
 * from across the room without reading a word.
 *
 * Three slots exist, so a game picks three: `small` is the dense cut for tiles
 * and stat lines, `body` is rows and labels, `title` is the header band.
 */
struct Faces {
  int small = kTileFontId;
  int body = kUiFontId;
  int title = kDisplayFontId;
};

/** The fork's default, and what Solitaire uses. */
inline Faces toyboxFaces() { return Faces{}; }

/**
 * @brief Connections' faces: a condensed serif that fits a long word in a tile.
 *
 * Upstream picks Instrument Serif because it is the only elegant face also
 * condensed enough to set "ACTUALLY" inside a 111px tile. That face is not
 * available here (see ToyboxFonts.h), so both bind to Literata — E-inx's own
 * reading face — at the two sizes that correspond. The header band stays in the
 * display cut either way: the top bar is the fork's chrome, not the game's
 * voice, and a shared one is what makes two apps feel like one device.
 */
inline Faces serifBoardFaces() { return Faces{kSerifTileFontId, kUiFontId, kDisplayFontId}; }
inline Faces serifMenuFaces() { return Faces{kSerifSmallFontId, kSerifTileFontId, kDisplayFontId}; }

/** For a screen whose surface is a page of prose rather than a board. */
inline Faces readingFaces() { return Faces{kTileFontId, kReadingFontId, kDisplayFontId}; }

/** Same, for a screen with buttons on it: the small slot carries the button cut. */
inline Faces readingChromeFaces() { return Faces{kButtonFontId, kReadingFontId, kDisplayFontId}; }

/** For a header band carrying a story's title rather than the app's name. */
inline Faces readerFaces() { return Faces{kButtonFontId, kReadingFontId, kReadingBoldFontId}; }

/** Builds a draw target with the three slots bound. */
inline einxui::EInxDrawTarget makeTarget(const GfxRenderer& renderer, const Faces& faces = Faces{}) {
  einxui::EInxDrawTarget target(renderer);
  // The small slot carries the dense cut, not a small UI face; see ToyboxTokens.h.
  target.setFont(einxui::EInxDrawTarget::FONT_SMALL, faces.small);
  target.setFont(einxui::EInxDrawTarget::FONT_BODY, faces.body);
  target.setFont(einxui::EInxDrawTarget::FONT_TITLE, faces.title);
  return target;
}

/**
 * @brief Notices a screen that registered more controls than the buffer holds.
 *
 * Overflow means a control drew but registered no hit rect, which reads to a
 * user as a dead button and to a developer as nothing at all. Cheap to check
 * once per paint; the alternative is finding it by pressing every button.
 */
inline void reportOverflow(const Interactions& interactions, const char* screenName) {
  if (interactions.overflowed()) {
    Serial.printf("[TOYBOX] %s registered more than %d interactions; some controls are unreachable\n", screenName,
                  static_cast<int>(kMaxInteractions));
  }
}

/** The rule under the header band, for callers drawing straight to the renderer. */
inline void headerRule(const GfxRenderer& renderer) {
  renderer.rectangle.fill(0, kHeaderHeight + 4, renderer.getScreenWidth(), kRule,
                          static_cast<int>(GfxRenderer::FillTone::Ink));
}

}  // namespace toybox
