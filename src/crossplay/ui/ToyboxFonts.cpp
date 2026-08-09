#include "ToyboxFonts.h"

namespace toybox {

FontMetrics metricsFor(const GfxRenderer& renderer, const int fontId) {
  FontMetrics metrics;
  metrics.ascender = renderer.text.getFontAscenderSize(fontId);
  // Distance from the ascender line down to the top of a capital 'H'. Flat-
  // topped and with no overshoot, so it measures the band the eye aligns to
  // rather than the box the font reserves.
  metrics.capTop = metrics.ascender - renderer.text.getGlyphTopInset(fontId, 'H');
  metrics.capHeight = metrics.capTop;
  return metrics;
}

int drawCapsCentered(const GfxRenderer& renderer, const int fontId, const int x, const int boxY, const int boxH,
                     const char* text, const bool black) {
  const FontMetrics metrics = metricsFor(renderer, fontId);
  // Ink top on screen is (y + ascender) - capTop. Solve for the y that puts the
  // ink top at the box's centred position.
  const int y = boxY + (boxH - metrics.capHeight) / 2 - metrics.ascender + metrics.capTop;
  renderer.text.render(fontId, x, y, text, black);
  return x;
}

}  // namespace toybox
