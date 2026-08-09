#pragma once

/**
 * @file StudyFonts.h
 * @brief The five CJK faces — declared, and deliberately never loaded.
 *
 * Upstream loads five subset Chinese faces (SimSun, SimHei, MicrosoftYaHei,
 * KaiTi, FangSong) off the card and randomises between them per card, so a
 * learner does not lock onto one letterform. That is a genuinely good idea and
 * it is not ported, for the same structural reason the Toybox typefaces are not:
 * the files are CrossPoint `.cpfont` blobs in CrossPoint's `EpdFontData` layout,
 * which is not E-inx's, and they arrive through `SdCardFontManager` /
 * `SdCardFontRegistry`, which E-inx does not have — its SD font system is its
 * own, with a different API and a different on-disk format.
 *
 * So this reports "nothing loaded", always. That is not a crash path: upstream
 * already handles a partial font install — the browser demo ships one face on
 * purpose — and `StudyActivity` reads `fontsReady_` and falls back to the
 * reading and meaning cuts. What it costs is real and worth stating plainly:
 *
 *   **A deck whose headwords are Chinese will draw them as tofu.** The pinyin,
 *   the gloss and the sentence are fine; the hanzi are boxes. Latin-script decks
 *   are unaffected and the scheduler, the deck parsing and the review flow all
 *   work as upstream intends.
 *
 * Making this real means running the deck's glyphs through E-inx's own `fontcon`
 * pipeline and rewriting the loader against `FontManager`. It is a self-contained
 * follow-up and this file is the only one that would change.
 */

#include <cstdint>

class GfxRenderer;

namespace study {

class StudyFonts {
 public:
  static constexpr int kFamilyCount = 5;

  /** The five families, in the order the card template lists them. */
  static const char* familyName(int index);

  /** Always false: there is no loader behind this. */
  bool load(GfxRenderer& renderer, int familyIndex);

  /** Always -1, meaning "none of the five are there" — a state upstream expects. */
  int loadPreferred(GfxRenderer& renderer, int preferred);

  void unload(GfxRenderer& renderer);

  /** No resident font to pull glyphs for. */
  void prewarm(GfxRenderer& renderer, const char* headword, const char* sentence) const;

  bool loaded() const { return false; }
  int headwordFontId() const { return 0; }
  int sentenceFontId() const { return 0; }
  int familyIndex() const { return -1; }
  const char* familyName() const { return familyName(familyIndex()); }
};

}  // namespace study
