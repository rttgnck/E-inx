#pragma once

// Toybox: the visual language CrossPlay's apps are drawn in.
//
// Ported from CrossPlay's src/apps_local/ui/Toybox.h. The shapes and the reasoning
// are theirs and are kept; what changed is the renderer underneath, because
// E-inx splits GfxRenderer into sub-renderers (renderer.rectangle.fill) where
// CrossPoint has one flat surface (renderer.fillRect), and because E-inx's
// rounded-rect primitive derives its radius from the box instead of taking one.
//
// The one rule everything else follows:
//
//   THE BLACK YOU CAN AFFORD IS INVERSELY PROPORTIONAL TO HOW OFTEN IT CHANGES.
//
// A solid black header that never repaints costs nothing: e-ink holds it at
// zero power and it never ghosts. The same amount of black in a play area that
// repaints every move ghosts badly and slows the refresh. So: solid fills for
// static chrome, dither and heavy outlines for anything that moves. That is
// what lets this look loud without behaving badly.
//
// This is deliberately NOT an E-inx UiTheme. The reader's chrome is the
// reader's; this applies to src/crossplay/ only.

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdint>

#include "../compat/CrossPlayRect.h"
#include "ToyboxMetrics.h"

namespace toybox {

namespace detail {
constexpr int kInk = static_cast<int>(GfxRenderer::FillTone::Ink);
constexpr int kPaper = static_cast<int>(GfxRenderer::FillTone::Paper);
}  // namespace detail

// A full-width horizontal rule. Heavy by default: hairlines read as timid here.
inline void rule(const GfxRenderer& renderer, const int y, const int weight = kRule) {
  renderer.rectangle.fill(kMargin, y, renderer.getScreenWidth() - 2 * kMargin, weight, detail::kInk);
}

// A gear, drawn rather than stored: at this size it is a ring with eight teeth,
// and a procedural one scales with the chrome without another asset to keep in
// step with the piece set.
//
// Upstream passes an explicit corner radius to every fillRoundedRect here. E-inx
// picks the radius from the box, so the calls below ask for `rounded` and let it
// choose. At these sizes (a few pixels square) both land on the same 1-2px
// curve, so the teeth and the hub look as intended; the hub's `body` radius,
// which upstream uses to turn a square into a circle, becomes the renderer's
// standard curve instead and reads as a rounded square. Accepted: the gear is a
// 24px settings glyph, not artwork.
inline void gear(const GfxRenderer& renderer, const Rect& box, const bool ink) {
  const int cx = box.x + box.width / 2;
  const int cy = box.y + box.height / 2;
  const int outer = box.width / 2;
  const int tooth = std::max(3, outer / 3);
  const int body = outer - tooth / 2;
  const int inkTone = ink ? detail::kInk : detail::kPaper;
  const int paperTone = ink ? detail::kPaper : detail::kInk;

  // Teeth first, so the body's edge cleans up where they meet it.
  for (int i = 0; i < 8; ++i) {
    static constexpr int kDx[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    static constexpr int kDy[8] = {-1, -1, 0, 1, 1, 1, 0, -1};
    // Diagonal teeth sit closer in, so all eight land on the same circle.
    const int reach = (kDx[i] != 0 && kDy[i] != 0) ? (body * 7) / 10 : body;
    const int tx = cx + kDx[i] * reach - tooth / 2;
    const int ty = cy + kDy[i] * reach - tooth / 2;
    renderer.rectangle.fill(tx, ty, tooth, tooth, inkTone, /*rounded=*/true, /*subtle=*/true);
  }
  renderer.rectangle.fill(cx - body, cy - body, body * 2, body * 2, inkTone, /*rounded=*/true);
  const int hole = std::max(2, body / 2);
  renderer.rectangle.fill(cx - hole, cy - hole, hole * 2, hole * 2, paperTone, /*rounded=*/true);
}

// Four corner marks inside a rect. Used to flag a square without covering what
// is standing on it, and it rhymes with the brackets around the board.
inline void cornerMarks(const GfxRenderer& renderer, const Rect& box, const int arm, const int weight) {
  const int x = box.x;
  const int y = box.y;
  const int w = box.width;
  const int h = box.height;
  renderer.rectangle.fill(x, y, arm, weight, detail::kInk);
  renderer.rectangle.fill(x, y, weight, arm, detail::kInk);
  renderer.rectangle.fill(x + w - arm, y, arm, weight, detail::kInk);
  renderer.rectangle.fill(x + w - weight, y, weight, arm, detail::kInk);
  renderer.rectangle.fill(x, y + h - weight, arm, weight, detail::kInk);
  renderer.rectangle.fill(x, y + h - arm, weight, arm, detail::kInk);
  renderer.rectangle.fill(x + w - arm, y + h - weight, arm, weight, detail::kInk);
  renderer.rectangle.fill(x + w - weight, y + h - arm, weight, arm, detail::kInk);
}

// Blits a 1bpp bitmap, MSB first, row-major, bit set = ink. CrossPlay's own asset
// format. Deliberately not IconRender, which bakes in an orientation meant for
// the reader's themed lists and would turn a card's pip on its side.
// `turned` draws the sprite rotated 180 degrees, for artwork that has to read
// right to somebody sitting on the other side of the device. A rotation rather
// than a second asset: at 1 bit there is nothing to resample, so reading the
// same bits from the far corner is exact.
inline void blit1bpp(const GfxRenderer& renderer, const uint8_t* bitmap, const int size, const int x, const int y,
                     const bool ink = true, const bool turned = false) {
  const int rowBytes = (size + 7) / 8;
  for (int row = 0; row < size; ++row) {
    for (int col = 0; col < size; ++col) {
      if ((bitmap[row * rowBytes + (col >> 3)] >> (7 - (col & 7))) & 1) {
        const int px = turned ? size - 1 - col : col;
        const int py = turned ? size - 1 - row : row;
        renderer.drawPixel(x + px, y + py, ink);
      }
    }
  }
}

}  // namespace toybox
