#include "EInxDrawTarget.h"

#include <cstring>

namespace einxui {

namespace fui = freeink::ui;

std::vector<std::string> wrapText(const GfxRenderer& renderer, const int fontId, const char* text, const int maxWidth,
                                  const int maxLines, const EpdFontFamily::Style style) {
  std::vector<std::string> lines;
  if (!text || text[0] == '\0' || maxWidth <= 0 || maxLines <= 0) return lines;

  // Greedy word wrap, breaking on spaces and honouring hard newlines. A word
  // wider than the whole line is broken at the last character that fits rather
  // than allowed to overhang, which on a 528px panel is the difference between
  // a long card title wrapping and it running off the edge.
  std::string current;
  const char* cursor = text;

  const auto flush = [&]() {
    lines.push_back(current);
    current.clear();
  };

  while (*cursor != '\0' && static_cast<int>(lines.size()) < maxLines) {
    // Take the next word plus any run of spaces that follows it.
    //
    // Named `token`, not `word`: Arduino.h defines `word(...)` as a function-like
    // macro, so a local of that name is replaced mid-expression and the error
    // lands on the ternary two tokens later with no mention of the cause.
    const char* wordEnd = cursor;
    while (*wordEnd != '\0' && *wordEnd != ' ' && *wordEnd != '\n') ++wordEnd;
    const std::string token(cursor, static_cast<size_t>(wordEnd - cursor));

    const std::string candidate = current.empty() ? token : current + " " + token;
    if (renderer.text.getWidth(fontId, candidate.c_str(), style) <= maxWidth) {
      current = candidate;
    } else if (current.empty()) {
      // The word alone overflows: break it character by character. Advancing by
      // whole UTF-8 sequences rather than bytes keeps a multi-byte glyph from
      // being split into two invalid ones.
      std::string piece;
      const char* charCursor = cursor;
      while (charCursor < wordEnd) {
        int charLen = 1;
        const auto lead = static_cast<unsigned char>(*charCursor);
        if ((lead & 0xE0) == 0xC0) charLen = 2;
        else if ((lead & 0xF0) == 0xE0) charLen = 3;
        else if ((lead & 0xF8) == 0xF0) charLen = 4;
        if (charCursor + charLen > wordEnd) charLen = 1;

        const std::string next = piece + std::string(charCursor, static_cast<size_t>(charLen));
        if (!piece.empty() && renderer.text.getWidth(fontId, next.c_str(), style) > maxWidth) break;
        piece = next;
        charCursor += charLen;
      }
      if (piece.empty()) break;  // Not even one glyph fits; stop rather than spin.
      current = piece;
      cursor = charCursor;
      flush();
      continue;
    } else {
      flush();
      continue;  // Retry the same word on the fresh line.
    }

    cursor = wordEnd;
    if (*cursor == '\n') {
      flush();
      ++cursor;
    } else {
      while (*cursor == ' ') ++cursor;
    }
  }

  if (!current.empty() && static_cast<int>(lines.size()) < maxLines) lines.push_back(current);
  return lines;
}

void EInxDrawTarget::fill(const fui::Rect rect, const fui::Paint paint, const uint8_t radius, const uint8_t) {
  if (rect.empty()) return;

  switch (paint.kind) {
    case fui::PaintKind::Solid:
    case fui::PaintKind::Dither: {
      if (paint.color == fui::Color::Transparent) break;
      renderer_.rectangle.fill(rect.x, rect.y, rect.width, rect.height, fillTone(paint.color), radius > 0,
                               subtleFor(radius));
      break;
    }
    case fui::PaintKind::Bitmap:
      bitmap(rect, paint.bitmap.bitmap, paint.bitmap.mode, fui::Paint::solid(fui::Color::Black));
      break;
    case fui::PaintKind::None:
    default:
      break;
  }
}

void EInxDrawTarget::stroke(const fui::Rect rect, const fui::Paint paint, const uint8_t width, const uint8_t radius,
                            const uint8_t) {
  if (rect.empty() || width == 0 || paint.kind == fui::PaintKind::None) return;
  const bool black = paint.color != fui::Color::White;
  const bool rounded = radius > 0;
  const bool subtle = subtleFor(radius);

  // No stroke width in this renderer, so thickness is nested outlines. Insetting
  // one pixel at a time keeps the outer edge where the caller put it, which is
  // what every layout below assumes when it measures a bordered box.
  for (uint8_t i = 0; i < width; ++i) {
    const int inset = i;
    const int w = rect.width - 2 * inset;
    const int h = rect.height - 2 * inset;
    if (w <= 0 || h <= 0) break;
    renderer_.rectangle.render(rect.x + inset, rect.y + inset, w, h, black, rounded, subtle);
  }
}

void EInxDrawTarget::line(const fui::Point from, const fui::Point to, const uint8_t width, const fui::Paint paint) {
  if (paint.kind == fui::PaintKind::None) return;
  const bool black = paint.color != fui::Color::White;
  const uint8_t thickness = width == 0 ? 1 : width;

  // Thickness by repetition, offset perpendicular to the run. Choosing the axis
  // from the dominant delta means a near-horizontal rule thickens downward and a
  // near-vertical one sideways, which is what a caller drawing a 3px divider
  // means by it.
  const int dx = to.x - from.x;
  const int dy = to.y - from.y;
  const bool steep = (dy < 0 ? -dy : dy) > (dx < 0 ? -dx : dx);

  for (uint8_t i = 0; i < thickness; ++i) {
    const int offset = i;
    if (steep) {
      renderer_.line.render(from.x + offset, from.y, to.x + offset, to.y, black);
    } else {
      renderer_.line.render(from.x, from.y + offset, to.x, to.y + offset, black);
    }
  }
}

void EInxDrawTarget::triangle(const fui::Point a, const fui::Point b, const fui::Point c, const fui::Paint paint) {
  if (paint.kind == fui::PaintKind::None) return;
  const int xs[3] = {a.x, b.x, c.x};
  const int ys[3] = {a.y, b.y, c.y};
  renderer_.polygon.render(xs, ys, 3, /*filled=*/true, paint.color != fui::Color::White);
}

void EInxDrawTarget::text(const fui::Rect rect, const char* text, const fui::TextStyle style) {
  if (!text || rect.empty()) return;
  const int fontId = gfxFont(style.font);
  const EpdFontFamily::Style epdStyle = fontStyle(style);
  const bool black = !style.inverted && style.color != fui::Color::White;
  const int lh = renderer_.text.getLineHeight(fontId);
  const uint8_t maxLines = style.maxLines > 0 ? style.maxLines : 1;

  if (style.rotation == fui::Rotation::CW90) {
    // E-inx renders rotated text upward from the given y, so the y positions the
    // *end* of the run and alignment offsets it. Rotations other than CW90 have
    // no native path and fall through to unrotated below: visibly wrong but
    // legible, which beats drawing nothing.
    const std::string textLine = renderer_.text.truncate(fontId, text, rect.height, epdStyle);
    const int textLen = renderer_.text.getWidth(fontId, textLine.c_str(), epdStyle);
    int y = rect.y + textLen;
    if (style.align == fui::TextAlign::Center) y = rect.y + (rect.height + textLen) / 2;
    if (style.align == fui::TextAlign::Right) y = rect.bottom();
    const int x = rect.x + std::max(0, (rect.width - lh) / 2);
    renderer_.text.rotated90CW(fontId, x, y, textLine.c_str(), black, epdStyle);
    return;
  }

  const auto drawAligned = [&](const std::string& textLine, const int y) {
    int x = rect.x;
    if (style.align != fui::TextAlign::Left) {
      const int textW = renderer_.text.getWidth(fontId, textLine.c_str(), epdStyle);
      x = style.align == fui::TextAlign::Center ? rect.x + (rect.width - textW) / 2 : rect.x + rect.width - textW;
      if (x < rect.x) x = rect.x;
    }
    renderer_.text.render(fontId, x, y, textLine.c_str(), black, epdStyle);
  };

  if (maxLines == 1) {
    const std::string textLine = renderer_.text.truncate(fontId, text, rect.width, epdStyle);
    drawAligned(textLine, rect.y + std::max(0, (rect.height - lh) / 2));
    return;
  }

  const std::vector<std::string> lines = wrapText(renderer_, fontId, text, rect.width, maxLines, epdStyle);
  const int blockH = static_cast<int>(lines.size()) * lh;
  int y = rect.y + std::max(0, (rect.height - blockH) / 2);
  for (const auto& textLine : lines) {
    drawAligned(textLine, y);
    y += lh;
  }
}

void EInxDrawTarget::bitmap(const fui::Rect rect, const fui::BitmapRef bitmap, const fui::BitmapMode mode,
                            const fui::Paint foreground, const fui::Rotation rotation) {
  if (!bitmap || rect.empty()) return;
  // BW1 (set bit = ink) and Mask1 (freeink::Icon: bit 0 = ink) are the two
  // formats the SDK's sampler folds together. Anything else would need its own
  // decode and nothing in this port produces one.
  if (bitmap.format != fui::BitmapFormat::BW1 && bitmap.format != fui::BitmapFormat::Mask1) return;

  const bool black = foreground.color != fui::Color::White;
  // Per-pixel is adequate here: these are 32px icons and card pips, not photos,
  // and the SDK's sampler already handles every BitmapMode's scaling and tiling.
  fui::forEachBitmapPixel(
      rect, bitmap, mode, [&](const int16_t px, const int16_t py) { renderer_.drawPixel(px, py, black); }, rotation);
}

}  // namespace einxui
