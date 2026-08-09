#pragma once

/**
 * @file CrossPlayTheme.h
 * @brief The few pieces of CrossPoint's reader theme the ported apps borrow.
 *
 * Almost everything under src/crossplay/ draws in Toybox, which is the point of
 * Toybox: the apps look like each other rather than like the reader. Study is
 * the exception — it borrows the reader's own header band and its wrapped-text
 * helper, because a flashcard is a page of prose and should look like one.
 *
 * E-inx has both of those things under different names and shapes: `UiTheme`
 * exposes its metrics as constants rather than a struct, and has no
 * centred-wrapped-text helper at all. This is the join.
 */

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "system/UiTheme.h"
#include "CrossPlayGfx.h"
#include "CrossPlayRect.h"
#include "EInxDrawTarget.h"

/**
 * @brief CrossPoint's theme façade, over E-inx's.
 *
 * Only the two entry points Study uses. Upstream's ThemeMetrics carries about
 * twenty fields; the two that are read here are the two that are here.
 */
class UITheme {
 public:
  struct Metrics {
    int topPadding;
    int headerHeight;
  };

  static UITheme& getInstance() {
    static UITheme instance;
    return instance;
  }

  Metrics getMetrics() const { return Metrics{UiTheme::TOP_STATUS_HEIGHT, UiTheme::DRAWER_HEADER_HEIGHT}; }

  /**
   * @brief Wraps `text` into `bounds` and centres the block, both axes.
   *
   * E-inx has no equivalent, so this is built on the same word-wrap the
   * FreeInkUI draw target uses — one implementation of "break this text", not
   * two that disagree at the edges.
   */
  static void drawCenteredWrappedText(const GfxRenderer& renderer, const Rect& bounds, const int fontId,
                                      const char* text, const int maxLines) {
    if (!text || bounds.width <= 0) return;
    const std::vector<std::string> lines =
        einxui::wrapText(renderer, fontId, text, bounds.width, maxLines, EpdFontFamily::REGULAR);
    if (lines.empty()) return;

    const int lineHeight = renderer.text.getLineHeight(fontId);
    const int blockHeight = static_cast<int>(lines.size()) * lineHeight;
    int y = bounds.y + (bounds.height - blockHeight) / 2;
    if (y < bounds.y) y = bounds.y;

    for (const std::string& line : lines) {
      const int width = renderer.text.getWidth(fontId, line.c_str());
      renderer.text.render(fontId, bounds.x + (bounds.width - width) / 2, y, line.c_str());
      y += lineHeight;
    }
  }
};
