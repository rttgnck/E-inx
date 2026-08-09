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
inline void ensureFonts(const GfxRenderer&) {}

/**
 * @brief Vertical metrics of the actual ink, which is not what the renderer reports.
 *
 * `getLineHeight()` and the y that `text.render()` takes are both measured from
 * the ascender box. Centring on that centres the box, not the letters: capital
 * ink sits at the bottom of the ascender box, so text visibly hangs low in every
 * bar and capsule. Centring on cap height is what typesetters actually do, and
 * it is what upstream's chrome is built around.
 */
struct FontMetrics {
  int ascender = 0;   ///< baseline = render y + ascender
  int capTop = 0;     ///< baseline up to the top of a capital
  int capHeight = 0;  ///< ink height of a capital
};

/**
 * @brief Cap metrics for a font id.
 *
 * Upstream reads them straight off its own `EpdFontData` tables, which it can
 * because it owns the twelve fonts it registers. Here the fonts are E-inx's
 * built-ins, held by the renderer, so the ascender comes from the renderer and
 * the cap band is derived from it.
 *
 * 'H' is the right glyph to ask about — flat-topped, no overshoot, so it
 * measures the band the eye aligns to — but E-inx's TextRender exposes no glyph
 * query beyond `getGlyphTopInset()`, which is exactly the ascender-to-ink
 * distance needed. Cap height is then the remaining ink below that inset, down
 * to the baseline.
 */
FontMetrics metricsFor(const GfxRenderer& renderer, int fontId);

/**
 * @brief Draws `text` with its capital ink vertically centred in [boxY, boxY + boxH).
 *
 * Returns the x it started at, so callers can chain.
 */
int drawCapsCentered(const GfxRenderer& renderer, int fontId, int x, int boxY, int boxH, const char* text, bool black);

}  // namespace toybox
