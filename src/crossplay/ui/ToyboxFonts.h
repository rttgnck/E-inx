#pragma once

/**
 * @file ToyboxFonts.h
 * @brief Which typefaces CrossPlay's screens speak in, on E-inx.
 *
 * Upstream ships twelve generated font headers — Jersey 25 and Noto Serif, cut
 * at 10/14/20/30px and converted at 1 bit — and registers them from its own
 * code. None of them can be used here, for a reason that is structural rather
 * than a matter of effort: CrossPoint's EpdGlyph carries `uint16_t advanceX` in
 * 12.4 fixed-point alongside kerning-class tables, ligature pairs and
 * DEFLATE-compressed glyph groups, and E-inx's carries `uint8_t advanceX` in
 * whole pixels and none of the rest. The generated arrays are not the same
 * binary format and cannot be re-pointed at a different struct.
 *
 * So the slots bind to E-inx's own built-ins instead. What that costs, stated
 * plainly so nobody spends an afternoon rediscovering it:
 *
 *   - Size. Upstream's display cut is 30px and its UI cut 20px; E-inx's largest
 *     built-in UI face is 18px. Header titles are therefore smaller relative to
 *     their band than the design intends. The bands themselves are unchanged, so
 *     a title sits in more air than it should.
 *   - Face. Jersey 25 is a condensed pixel face picked because it is 1-bit and
 *     cannot flood; Atkinson Hyperlegible is a 2-bit antialiased face. On the BW
 *     path any coverage above zero paints a pixel, so this type is heavier than
 *     upstream's and slightly muddier at the smallest cut. It is what the rest
 *     of E-inx already reads in, which is its own argument.
 *
 * Regenerating Jersey in E-inx's format with the fontcon tooling is the fix, and
 * it is a self-contained follow-up: this header is the only place that would
 * change.
 */

#include <GfxRenderer.h>

#include "system/Fonts.h"

namespace toybox {

// The display cut: header bands. Bold, and the largest UI face E-inx builds in.
constexpr int kDisplayFontId = ATKINSON_HYPERLEGIBLE_18_FONT_ID;
// The UI cut: rows, labels, buttons — most of what a screen says.
constexpr int kUiFontId = ATKINSON_HYPERLEGIBLE_16_FONT_ID;
// The dense cut: stat lines, captions, and anything set inside a tile.
constexpr int kTileFontId = ATKINSON_HYPERLEGIBLE_10_FONT_ID;

// Upstream distinguishes a serif reading face from the UI face for its prose
// apps (Hacker News, xkcd). Literata is what E-inx already sets books in, so it
// is the honest counterpart. Unused by Solitaire; here so the apps that follow
// have somewhere to bind rather than each inventing one.
constexpr int kSerifTitleFontId = LITERATA_18_FONT_ID;
constexpr int kSerifTileFontId = LITERATA_12_FONT_ID;
constexpr int kSerifSmallFontId = LITERATA_10_FONT_ID;
constexpr int kReadingFontId = LITERATA_14_FONT_ID;
constexpr int kReadingBoldFontId = LITERATA_16_FONT_ID;
constexpr int kReadingSmallFontId = LITERATA_12_FONT_ID;
constexpr int kReadingBoldSmallFontId = LITERATA_12_FONT_ID;
constexpr int kButtonFontId = ATKINSON_HYPERLEGIBLE_14_FONT_ID;

/**
 * @brief Called from an activity's onEnter() before drawing.
 *
 * Upstream registers its twelve EpdFonts with the renderer here. E-inx's
 * built-ins are already registered at boot, so there is nothing to do — but the
 * call stays, and stays at every call site, because it is the hook a future
 * Jersey port needs and removing it now would mean putting it back into every
 * ported app later.
 */
inline void ensureFonts(GfxRenderer&) {}

}  // namespace toybox
