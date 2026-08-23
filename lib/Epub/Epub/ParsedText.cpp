/**
 * @file ParsedText.cpp
 * @brief Definitions for ParsedText.
 */

#include "ParsedText.h"

#include <GfxRenderer.h>
#include <Utf8.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <type_traits>
#include <limits>
#include <new>

#ifndef SIMULATOR
#include <esp_heap_caps.h>
#endif

namespace {
/** Free bytes and the largest single block, or zeroes where the platform cannot say. */
unsigned freeHeapBytes() {
#ifdef SIMULATOR
  return 0;
#else
  return static_cast<unsigned>(ESP.getFreeHeap());
#endif
}
unsigned largestFreeBlockBytes() {
#ifdef SIMULATOR
  return 0;
#else
  return static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
}
}  // namespace
#include <vector>

#include "hyphenation/Hyphenator.h"

constexpr int MAX_COST = std::numeric_limits<int>::max();

namespace {

constexpr char SOFT_HYPHEN_UTF8[] = "\xC2\xAD";
constexpr size_t SOFT_HYPHEN_BYTES = 2;
constexpr uint8_t kScriptScalePct = 70;

bool containsSoftHyphen(const std::string& word) { return word.find(SOFT_HYPHEN_UTF8) != std::string::npos; }

bool isUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t nextUtf8CodepointOffset(const std::string& word, size_t offset) {
  if (offset >= word.size()) {
    return word.size();
  }
  ++offset;
  while (offset < word.size() && isUtf8Continuation(static_cast<unsigned char>(word[offset]))) {
    ++offset;
  }
  return offset;
}

uint8_t bionicPrefixLengthBytes(const std::string& word) {
  size_t codepoints = 0;
  for (size_t i = 0; i < word.size(); i = nextUtf8CodepointOffset(word, i)) {
    ++codepoints;
  }
  if (codepoints < 4) {
    return 0;
  }

  const size_t prefixCodepoints = std::min<size_t>(4, std::max<size_t>(1, (codepoints + 1) / 2));
  size_t offset = 0;
  for (size_t i = 0; i < prefixCodepoints && offset < word.size(); ++i) {
    offset = nextUtf8CodepointOffset(word, offset);
  }
  return static_cast<uint8_t>(std::min<size_t>(offset, 255));
}

uint16_t measureSmallCapsWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                                   const EpdFontFamily::Style style) {
  return static_cast<uint16_t>(std::max(0, renderer.text.getSmallCapsWidth(fontId, word.c_str(), style)));
}

EpdFontFamily::Style bionicStyleFor(EpdFontFamily::Style style) {
  switch (style) {
    case EpdFontFamily::ITALIC:
      return EpdFontFamily::BOLD_ITALIC;
    case EpdFontFamily::REGULAR:
      return EpdFontFamily::BOLD;
    case EpdFontFamily::BOLD:
    case EpdFontFamily::BOLD_ITALIC:
    default:
      return style;
  }
}

void stripSoftHyphensInPlace(std::string& word) {
  size_t pos = 0;
  while ((pos = word.find(SOFT_HYPHEN_UTF8, pos)) != std::string::npos) {
    word.erase(pos, SOFT_HYPHEN_BYTES);
  }
}

uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const bool smallCaps = false,
                          const bool appendHyphen = false) {
  const bool hasSoftHyphen = containsSoftHyphen(word);
  if (!hasSoftHyphen && !appendHyphen) {
    return smallCaps ? measureSmallCapsWordWidth(renderer, fontId, word, style)
                     : renderer.text.getWidth(fontId, word.c_str(), style);
  }

  std::string sanitized = word;
  if (hasSoftHyphen) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }
  return smallCaps ? measureSmallCapsWordWidth(renderer, fontId, sanitized, style)
                   : renderer.text.getWidth(fontId, sanitized.c_str(), style);
}

uint16_t measureWordWidth(const GfxRenderer& renderer, const int fontId, const std::string& word,
                          const EpdFontFamily::Style style, const uint8_t bionicPrefixBytes, const bool smallCaps,
                          const bool appendHyphen = false) {
  if (bionicPrefixBytes == 0 || bionicPrefixBytes >= word.size()) {
    return measureWordWidth(renderer, fontId, word, style, smallCaps, appendHyphen);
  }

  std::string sanitized = word;
  if (containsSoftHyphen(sanitized)) {
    stripSoftHyphensInPlace(sanitized);
  }
  if (appendHyphen) {
    sanitized.push_back('-');
  }

  const uint8_t prefixBytes = static_cast<uint8_t>(std::min<size_t>(bionicPrefixBytes, sanitized.size()));
  const std::string prefix = sanitized.substr(0, prefixBytes);
  const std::string suffix = sanitized.substr(prefixBytes);
  return measureWordWidth(renderer, fontId, prefix, bionicStyleFor(style), smallCaps) +
         measureWordWidth(renderer, fontId, suffix, style, smallCaps);
}

uint16_t measureWordWidthForAlign(const GfxRenderer& renderer, const int fontId, const std::string& word,
                                  const EpdFontFamily::Style style, const uint8_t bionicPrefixBytes,
                                  const bool smallCaps, const uint8_t verticalAlign) {
  if (verticalAlign == TextBlock::SUPERSCRIPT || verticalAlign == TextBlock::SUBSCRIPT) {
    return static_cast<uint16_t>(
        std::max(0, renderer.text.getScaledWidth(fontId, word.c_str(), kScriptScalePct, style)));
  }
  return measureWordWidth(renderer, fontId, word, style, bionicPrefixBytes, smallCaps);
}

/**
 * When a drop cap / left indent is active, the optimal layout uses O(n^2) DP tables (~12 bytes per cell).
 * Long paragraphs (common in fixed-layout / image-heavy EPUBs) exhaust heap and abort(); greedy packing
 * matches the width rules used by hyphenated layout without O(n^2) memory.
 */
std::vector<size_t> computeGreedyLineBreaksWithDropIndent(const int pageWidth, const int spaceWidth,
                                                          const std::vector<uint16_t>& wordWidths,
                                                          const std::vector<uint8_t>& wordJoinPrevious,
                                                          const int dropIndentW, const int dropIndentLines) {
  std::vector<size_t> lineBreakIndices;
  const size_t n = wordWidths.size();
  const std::vector<uint8_t>& joinPrevious = wordJoinPrevious;
  size_t currentIndex = 0;
  int lineNum = 0;

  while (currentIndex < n) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    const int lineW = (dropIndentW > 0 && lineNum < dropIndentLines) ? pageWidth - dropIndentW : pageWidth;

    while (currentIndex < n) {
      const bool isFirstWord = currentIndex == lineStart;
      const bool joinedToPrevious = currentIndex < joinPrevious.size() && joinPrevious[currentIndex] != 0;
      const int spacing = (isFirstWord || joinedToPrevious) ? 0 : spaceWidth;
      const int candidateWidth = spacing + static_cast<int>(wordWidths[currentIndex]);

      if (lineWidth + candidateWidth <= lineW) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    lineBreakIndices.push_back(currentIndex);
    ++lineNum;
  }

  return lineBreakIndices;
}

}  // namespace

namespace {

/**
 * Far past any real paragraph. A block this long is a parser accident, and letting it keep
 * growing is how a single malformed chapter takes the whole reader down with it.
 */
constexpr size_t kMaxWordsPerBlock = 20000;

/**
 * Reserves `target` elements, first checking the allocation can actually be met.
 *
 * std::vector::reserve throws on failure, and on this device a throw at that moment is fatal:
 * there is no memory left to build the exception object with either, so the runtime gives up and
 * aborts. Probing with a nothrow allocation of the same size turns "reboot mid-book" into a
 * boolean the caller can act on. Nothing else allocates between the probe and the reserve — the
 * parse is single-threaded — so the answer still holds by the time it is used.
 */
template <typename Vector>
bool reserveOrFail(Vector& vector, const size_t target) {
  if (target <= vector.capacity()) {
    return true;
  }
  void* probe = ::operator new(target * sizeof(typename Vector::value_type), std::nothrow);
  if (probe == nullptr) {
    return false;
  }
  ::operator delete(probe);
  vector.reserve(target);
  return true;
}

}  // namespace

bool ParsedText::reserveForNextWord() {
  const size_t target = words.size() + 1;
  if (target > kMaxWordsPerBlock) {
    return false;
  }

  // Growth is geometric so this runs rarely, and every array is grown together: a block whose
  // arrays end up different lengths is one TextBlock::render refuses to draw at all.
  if (!reserveOrFail(words, target) || !reserveOrFail(wordStyles, target) ||
      !reserveOrFail(bionicPrefixBytes, target) || !reserveOrFail(wordSmallCaps, target) ||
      !reserveOrFail(wordUnderline, target) || !reserveOrFail(wordVerticalAlign, target) ||
      !reserveOrFail(wordJoinPrevious, target)) {
    return false;
  }

  if (hasInlineImages_) {
    if (!reserveOrFail(wordImagePaths, target) || !reserveOrFail(wordImageW, target) ||
        !reserveOrFail(wordImageH, target)) {
      return false;
    }
  }

  return true;
}

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle, const bool smallCaps,
                         const bool underline, const bool joinPrevious, const uint8_t verticalAlign) {
  if (word.empty()) return;

  if (!reserveForNextWord()) {
    if (!truncatedForMemory_) {
      truncatedForMemory_ = true;
      Serial.printf("[%lu] [PT] Text block truncated at %u words: free=%u largest=%u\n", millis(),
                    static_cast<unsigned>(words.size()), freeHeapBytes(), largestFreeBlockBytes());
    }
    return;
  }

  const uint8_t bionicPrefixBytesValue = bionicReadingEnabled ? bionicPrefixLengthBytes(word) : 0;
  words.push_back(std::move(word));
  wordStyles.push_back(fontStyle);
  bionicPrefixBytes.push_back(bionicPrefixBytesValue);
  wordSmallCaps.push_back(smallCaps ? 1 : 0);
  wordUnderline.push_back(underline ? 1 : 0);
  wordVerticalAlign.push_back(verticalAlign);
  wordJoinPrevious.push_back(joinPrevious && words.size() > 1 ? 1 : 0);
  // Only carry image-list entries once this block has an inline image (keeps plain text blocks lean).
  if (hasInlineImages_) {
    wordImagePaths.emplace_back();
    wordImageW.push_back(0);
    wordImageH.push_back(0);
  }
}

void ParsedText::addImage(std::string cachePath, const uint16_t displayW, const uint16_t displayH) {
  if (cachePath.empty() || displayW == 0 || displayH == 0) return;
  if (!reserveForNextWord()) {
    if (!truncatedForMemory_) {
      truncatedForMemory_ = true;
      Serial.printf("[%lu] [PT] Inline image dropped at %u words: free=%u largest=%u\n", millis(),
                    static_cast<unsigned>(words.size()), freeHeapBytes(), largestFreeBlockBytes());
    }
    return;
  }
  // First image in this block: backfill empty image slots for the words already added so the arrays
  // align. reserveForNextWord() above only covers the image arrays once they exist, so the room for
  // the backfill is claimed here — and if it cannot be, the block keeps its text and drops the image
  // rather than ending up with arrays of different lengths that render refuses to draw.
  if (!hasInlineImages_) {
    const size_t n = words.size();
    if (!reserveOrFail(wordImagePaths, n + 1) || !reserveOrFail(wordImageW, n + 1) ||
        !reserveOrFail(wordImageH, n + 1)) {
      if (!truncatedForMemory_) {
        truncatedForMemory_ = true;
        Serial.printf("[%lu] [PT] Inline image dropped at %u words: no room for image slots\n", millis(),
                      static_cast<unsigned>(n));
      }
      return;
    }
    wordImagePaths.assign(n, std::string());
    wordImageW.assign(n, 0);
    wordImageH.assign(n, 0);
    hasInlineImages_ = true;
  }
  // Placeholder text word (empty) keeps every parallel list aligned; the image fields carry the real data.
  words.emplace_back();
  wordStyles.push_back(EpdFontFamily::REGULAR);
  bionicPrefixBytes.push_back(0);
  wordSmallCaps.push_back(0);
  wordUnderline.push_back(0);
  wordVerticalAlign.push_back(TextBlock::BASELINE);
  wordJoinPrevious.push_back(0);
  wordImagePaths.push_back(std::move(cachePath));
  wordImageW.push_back(displayW);
  wordImageH.push_back(displayH);
}

void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  applyParagraphIndent(renderer, fontId);

  const int pageWidth = viewportWidth;
  // The word-spacing setting scales the natural inter-word space; it is baked into the line layout (xpos).
  const int spaceWidth =
      std::max(1, static_cast<int>(std::lround(renderer.text.getSpaceWidth(fontId) * wordSpacingFactor_)));
  auto wordWidths = calculateWordWidths(renderer, fontId);
  std::vector<size_t> lineBreakIndices;
  const int dropW = static_cast<int>(leftIndentWidth);
  const int dropL = static_cast<int>(leftIndentLineCount);
  if (hyphenationEnabled) {
    lineBreakIndices = computeHyphenatedLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, dropW, dropL);
  } else {
    lineBreakIndices = computeLineBreaks(renderer, fontId, pageWidth, spaceWidth, wordWidths, dropW, dropL);
  }
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;
  const std::vector<uint8_t> joinPreviousSnapshot(wordJoinPrevious.begin(), wordJoinPrevious.end());

  for (size_t i = 0; i < lineCount; ++i) {
    extractLine(i, pageWidth, spaceWidth, wordWidths, lineBreakIndices, joinPreviousSnapshot, processLine);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  const size_t totalWordCount = words.size();

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  // The attribute vectors are either empty or exactly as long as `words`, so each is indexed
  // behind its own bounds check rather than walked with a parallel cursor.
  for (size_t i = 0; i < totalWordCount; ++i) {
    const bool smallCaps = i < wordSmallCaps.size() && wordSmallCaps[i] != 0;
    const uint8_t verticalAlign = i < wordVerticalAlign.size() ? wordVerticalAlign[i] : TextBlock::BASELINE;

    if (i < wordImagePaths.size() && !wordImagePaths[i].empty()) {
      // Inline image: its on-line footprint is the image display width (no text measuring).
      wordWidths.push_back(i < wordImageW.size() ? wordImageW[i] : 0);
      continue;
    }

    const uint8_t bionicPrefix = i < bionicPrefixBytes.size() ? bionicPrefixBytes[i] : 0;
    wordWidths.push_back(
        measureWordWidthForAlign(renderer, fontId, words[i], wordStyles[i], bionicPrefix, smallCaps, verticalAlign));
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const GfxRenderer& renderer, const int fontId, const int pageWidth,
                                                  const int spaceWidth, std::vector<uint16_t>& wordWidths,
                                                  int dropIndentW, int dropIndentLines) {
  if (words.empty()) {
    return {};
  }

  /** Break words wider than the *tightest* column they may occupy (narrow drop-cap lines), not full pageWidth. */
  const int narrowColumn = (dropIndentW > 0 && dropIndentLines > 0) ? std::max(1, pageWidth - dropIndentW) : pageWidth;
  for (size_t i = 0; i < wordWidths.size(); ++i) {
    while (static_cast<int>(wordWidths[i]) > narrowColumn) {
      if (!hyphenateWordAtIndex(i, narrowColumn, renderer, fontId, wordWidths, true)) {
        break;
      }
    }
  }

  const int n = static_cast<int>(words.size());

  if (dropIndentW <= 0 || dropIndentLines <= 0) {
    const size_t totalWordCount = words.size();
    constexpr size_t kMaxOptimalLineBreakWords = 220;
    if (totalWordCount > kMaxOptimalLineBreakWords) {
      return computeGreedyLineBreaksWithDropIndent(pageWidth, spaceWidth, wordWidths, wordJoinPrevious, dropIndentW,
                                                   dropIndentLines);
    }

    std::vector<int> dp(totalWordCount);
    std::vector<size_t> ans(totalWordCount);
    dp[totalWordCount - 1] = 0;
    ans[totalWordCount - 1] = totalWordCount - 1;

    for (int i = static_cast<int>(totalWordCount) - 2; i >= 0; --i) {
      int currlen = 0;
      int naturalGapCount = 0;
      dp[static_cast<size_t>(i)] = MAX_COST;

      for (size_t j = static_cast<size_t>(i); j < totalWordCount; ++j) {
        const bool joinedToPrevious = j > static_cast<size_t>(i) && wordJoinPrevious[j] != 0;
        const int gap = (j == static_cast<size_t>(i) || joinedToPrevious) ? 0 : spaceWidth;
        currlen += wordWidths[j] + gap;
        if (gap > 0) {
          ++naturalGapCount;
        }
        // Only justified rendering compresses spaces; for left/center/right the line is drawn at natural
        // spacing, so over-packing it would overflow (and centering would shove the first words off-screen).
        const int compressBudget = (style == TextBlock::JUSTIFIED) ? (naturalGapCount * spaceWidth * 2) / 5 : 0;
        if (currlen > pageWidth + compressBudget) {
          break;
        }
        int cost;
        if (j == totalWordCount - 1) {
          cost = 0;
        } else {
          const int remainingSpace = pageWidth - currlen;
          // Penalize stretched lines (positive remaining = unnatural gaps) more than compressed ones so the
          // layout favors packing words tightly, while the budget above keeps spaces readable.
          const int penalty = remainingSpace >= 0 ? remainingSpace : (-remainingSpace) / 3;
          const long long cost_ll = static_cast<long long>(penalty) * penalty + dp[j + 1];
          cost = (cost_ll > MAX_COST) ? MAX_COST : static_cast<int>(cost_ll);
        }
        if (cost < dp[static_cast<size_t>(i)]) {
          dp[static_cast<size_t>(i)] = cost;
          ans[static_cast<size_t>(i)] = j;
        }
      }
      if (dp[static_cast<size_t>(i)] == MAX_COST) {
        ans[static_cast<size_t>(i)] = static_cast<size_t>(i);
        if (i + 1 < static_cast<int>(totalWordCount)) {
          dp[static_cast<size_t>(i)] = dp[static_cast<size_t>(i + 1)];
        } else {
          dp[static_cast<size_t>(i)] = 0;
        }
      }
    }

    std::vector<size_t> lineBreakIndices;
    size_t currentWordIndex = 0;
    while (currentWordIndex < totalWordCount) {
      size_t nextBreakIndex = ans[currentWordIndex] + 1;
      if (nextBreakIndex <= currentWordIndex) {
        nextBreakIndex = currentWordIndex + 1;
      }
      lineBreakIndices.push_back(nextBreakIndex);
      currentWordIndex = nextBreakIndex;
    }
    return lineBreakIndices;
  }

  /**
   * CSS first-line indent now routes through the same left-indent fields as drop caps.
   * Running the drop-indent DP for every ordinary indented paragraph is expensive and
   * can fragment / exhaust heap on real books. For one-line indents, greedy layout is
   * plenty stable and avoids the quadratic allocation entirely.
   */
  if (dropIndentLines <= 1) {
    return computeGreedyLineBreaksWithDropIndent(pageWidth, spaceWidth, wordWidths, wordJoinPrevious, dropIndentW,
                                                 dropIndentLines);
  }

  /** Drop-indent optimal DP is (n+1)*(n+2) cells * ~12 B — fails on long blocks (bad_alloc / abort). */
  constexpr size_t kMaxDropIndentDpCells = 4800;
  const size_t gridCells = static_cast<size_t>(n + 1) * static_cast<size_t>(n + 2);
  if (gridCells > kMaxDropIndentDpCells) {
    return computeGreedyLineBreaksWithDropIndent(pageWidth, spaceWidth, wordWidths, wordJoinPrevious, dropIndentW,
                                                 dropIndentLines);
  }

  const int maxEll = n + 1;

  std::vector<std::vector<int>> dp(static_cast<size_t>(n + 1),
                                   std::vector<int>(static_cast<size_t>(maxEll + 1), MAX_COST));
  std::vector<std::vector<size_t>> ans(static_cast<size_t>(n + 1),
                                       std::vector<size_t>(static_cast<size_t>(maxEll + 1), 0));

  for (int ell = 0; ell <= maxEll; ++ell) {
    dp[static_cast<size_t>(n)][static_cast<size_t>(ell)] = 0;
  }

  for (int i = n - 1; i >= 0; --i) {
    for (int ell = 0; ell <= n; ++ell) {
      const int W = (dropIndentW > 0 && ell < dropIndentLines) ? pageWidth - dropIndentW : pageWidth;

      int currlen = 0;
      dp[static_cast<size_t>(i)][static_cast<size_t>(ell)] = MAX_COST;

      for (int j = i; j < n; ++j) {
        const bool joinedToPrevious =
            j > i && wordJoinPrevious[j] != 0;
        currlen += wordWidths[static_cast<size_t>(j)] + ((j == i || joinedToPrevious) ? 0 : spaceWidth);
        if (currlen > W) {
          break;
        }

        int cost;
        if (j == n - 1) {
          cost = 0;
        } else {
          const int remainingSpace = W - currlen;
          const long long cost_ll =
              static_cast<long long>(remainingSpace) * remainingSpace +
              static_cast<long long>(dp[static_cast<size_t>(j + 1)][static_cast<size_t>(ell + 1)]);
          if (cost_ll > MAX_COST) {
            cost = MAX_COST;
          } else {
            cost = static_cast<int>(cost_ll);
          }
        }

        if (cost < dp[static_cast<size_t>(i)][static_cast<size_t>(ell)]) {
          dp[static_cast<size_t>(i)][static_cast<size_t>(ell)] = cost;
          ans[static_cast<size_t>(i)][static_cast<size_t>(ell)] = static_cast<size_t>(j);
        }
      }

      if (dp[static_cast<size_t>(i)][static_cast<size_t>(ell)] == MAX_COST) {
        ans[static_cast<size_t>(i)][static_cast<size_t>(ell)] = static_cast<size_t>(i);
        if (i + 1 < n) {
          dp[static_cast<size_t>(i)][static_cast<size_t>(ell)] =
              dp[static_cast<size_t>(i + 1)][static_cast<size_t>(ell + 1)];
        } else {
          dp[static_cast<size_t>(i)][static_cast<size_t>(ell)] = 0;
        }
      }
    }
  }

  std::vector<size_t> lineBreakIndices;
  size_t idx = 0;
  int ell = 0;
  while (idx < static_cast<size_t>(n)) {
    const size_t last = ans[idx][static_cast<size_t>(ell)];
    lineBreakIndices.push_back(last + 1);
    idx = last + 1;
    ++ell;
  }

  return lineBreakIndices;
}

void ParsedText::applyParagraphIndent(const GfxRenderer& renderer, const int fontId) {
  if (words.empty()) {
    return;
  }

  if (leftIndentWidth > 0 && leftIndentLineCount > 0) {
    return;
  }

  if (cssTextIndentPx > 0) {
    leftIndentWidth = static_cast<uint16_t>(std::min(cssTextIndentPx, 65535));
    leftIndentLineCount = 1;
    return;
  }

  if (!respectParagraphIndent_) {
    return;
  }

  // Don't indent a leading inline image word (its text slot must stay empty).
  const bool frontIsImage = !wordImagePaths.empty() && !wordImagePaths.front().empty();
  if ((style == TextBlock::JUSTIFIED || style == TextBlock::LEFT_ALIGN) && !frontIsImage) {
    const int emWidth = renderer.text.getWidth(fontId, "\xe2\x80\x83", EpdFontFamily::REGULAR);
    const int fallbackWidth = renderer.text.getSpaceWidth(fontId) * 2;
    leftIndentWidth = static_cast<uint16_t>(std::min(std::max(emWidth, fallbackWidth), 65535));
    leftIndentLineCount = 1;
  }
}

std::vector<size_t> ParsedText::computeHyphenatedLineBreaks(const GfxRenderer& renderer, const int fontId,
                                                            const int pageWidth, const int spaceWidth,
                                                            std::vector<uint16_t>& wordWidths, int dropIndentW,
                                                            int dropIndentLines) {
  std::vector<size_t> lineBreakIndices;
  size_t currentIndex = 0;
  int lineNum = 0;

  while (currentIndex < wordWidths.size()) {
    const size_t lineStart = currentIndex;
    int lineWidth = 0;
    const int lineW = (dropIndentW > 0 && lineNum < dropIndentLines) ? pageWidth - dropIndentW : pageWidth;

    while (currentIndex < wordWidths.size()) {
      const bool isFirstWord = currentIndex == lineStart;
      const bool joinedToPrevious =
          currentIndex < wordJoinPrevious.size() && wordJoinPrevious[currentIndex] != 0;
      const int spacing = (isFirstWord || joinedToPrevious) ? 0 : spaceWidth;
      const int candidateWidth = spacing + wordWidths[currentIndex];

      // Only justified rendering compresses spaces (see computeLineBreaks); other alignments draw at natural
      // spacing, so over-packing would overflow / push centered words off-screen.
      int naturalGapCount = 0;
      for (size_t gi = lineStart + 1; gi <= currentIndex; ++gi) {
        if (wordJoinPrevious[gi] == 0) {
          ++naturalGapCount;
        }
      }
      const int compressBudget = (style == TextBlock::JUSTIFIED) ? (naturalGapCount * spaceWidth * 2) / 5 : 0;
      if (lineWidth + candidateWidth <= lineW + compressBudget) {
        lineWidth += candidateWidth;
        ++currentIndex;
        continue;
      }

      const int availableWidth = lineW - lineWidth - spacing;
      const bool allowFallbackBreaks = isFirstWord;

      if (availableWidth > 0 &&
          hyphenateWordAtIndex(currentIndex, availableWidth, renderer, fontId, wordWidths, allowFallbackBreaks)) {
        lineWidth += spacing + wordWidths[currentIndex];
        ++currentIndex;
        break;
      }

      if (currentIndex == lineStart) {
        lineWidth += candidateWidth;
        ++currentIndex;
      }
      break;
    }

    lineBreakIndices.push_back(currentIndex);
    ++lineNum;
  }

  return lineBreakIndices;
}

bool ParsedText::hyphenateWordAtIndex(const size_t wordIndex, const int availableWidth, const GfxRenderer& renderer,
                                      const int fontId, std::vector<uint16_t>& wordWidths,
                                      const bool allowFallbackBreaks) {
  if (availableWidth <= 0 || wordIndex >= words.size()) {
    return false;
  }

  const bool blockHasImages = !wordImagePaths.empty();
  if (blockHasImages && !wordImagePaths[wordIndex].empty()) {
    // Inline images are atomic — never hyphenate / split them.
    return false;
  }

  // Indices, not iterators: the inserts further down reallocate, which would invalidate any
  // iterator taken before them.
  const std::string& word = words[wordIndex];
  const auto style = wordStyles[wordIndex];
  const bool smallCaps = wordSmallCaps[wordIndex] != 0;
  const bool underline = wordUnderline[wordIndex] != 0;
  const uint8_t verticalAlign = wordVerticalAlign[wordIndex];
  if (verticalAlign == TextBlock::SUPERSCRIPT || verticalAlign == TextBlock::SUBSCRIPT) {
    return false;
  }

  auto breakInfos = Hyphenator::breakOffsets(word, allowFallbackBreaks);
  if (breakInfos.empty()) {
    return false;
  }

  size_t chosenOffset = 0;
  int chosenWidth = -1;
  bool chosenNeedsHyphen = true;

  for (const auto& info : breakInfos) {
    const size_t offset = info.byteOffset;
    if (offset == 0 || offset >= word.size()) {
      continue;
    }

    const bool needsHyphen = info.requiresInsertedHyphen;
    const uint8_t hyphenPrefixBytes = bionicReadingEnabled ? bionicPrefixLengthBytes(word.substr(0, offset)) : 0;
    const int prefixWidth =
        measureWordWidth(renderer, fontId, word.substr(0, offset), style, hyphenPrefixBytes, smallCaps, needsHyphen);
    if (prefixWidth > availableWidth || prefixWidth <= chosenWidth) {
      continue;
    }

    chosenWidth = prefixWidth;
    chosenOffset = offset;
    chosenNeedsHyphen = needsHyphen;
  }

  if (chosenWidth < 0) {
    return false;
  }

  std::string remainder = word.substr(chosenOffset);
  words[wordIndex].resize(chosenOffset);
  if (chosenNeedsHyphen) {
    words[wordIndex].push_back('-');
  }

  // Each insert can reallocate, so every position is recomputed from wordIndex rather than held
  // across the previous insert.
  const auto at = [wordIndex](auto& container) { return container.begin() + static_cast<std::ptrdiff_t>(wordIndex) + 1; };

  const uint8_t prefixBionic = bionicReadingEnabled ? bionicPrefixLengthBytes(words[wordIndex]) : 0;
  const uint8_t remainderBionic = bionicReadingEnabled ? bionicPrefixLengthBytes(remainder) : 0;
  bionicPrefixBytes[wordIndex] = prefixBionic;

  words.insert(at(words), remainder);
  wordStyles.insert(at(wordStyles), style);
  bionicPrefixBytes.insert(at(bionicPrefixBytes), remainderBionic);
  wordSmallCaps.insert(at(wordSmallCaps), smallCaps ? 1 : 0);
  wordUnderline.insert(at(wordUnderline), underline ? 1 : 0);
  wordVerticalAlign.insert(at(wordVerticalAlign), verticalAlign);
  wordJoinPrevious.insert(at(wordJoinPrevious), 0);
  // The split halves are plain text — keep the parallel image lists aligned (only when this block has any).
  if (blockHasImages) {
    wordImagePaths.insert(at(wordImagePaths), std::string());
    wordImageW.insert(at(wordImageW), 0);
    wordImageH.insert(at(wordImageH), 0);
  }

  wordWidths[wordIndex] = static_cast<uint16_t>(chosenWidth);
  const uint16_t remainderWidth = measureWordWidth(renderer, fontId, remainder, style, remainderBionic, smallCaps);
  wordWidths.insert(wordWidths.begin() + wordIndex + 1, remainderWidth);
  return true;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<size_t>& lineBreakIndices,
                             const std::vector<uint8_t>& joinPreviousSnapshot,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  int lineWordWidthSum = 0;
  for (size_t i = lastBreakAt; i < lineBreak; i++) {
    lineWordWidthSum += wordWidths[i];
  }
  auto joinedAt = [&](const size_t index) -> bool {
    if (index == 0 || index >= joinPreviousSnapshot.size()) {
      return false;
    }
    return joinPreviousSnapshot[index] != 0;
  };
  int naturalGapCount = 0;
  for (size_t i = lastBreakAt + 1; i < lineBreak; ++i) {
    if (!joinedAt(i)) {
      ++naturalGapCount;
    }
  }

  // Track the widest natural (pre-alignment) line so CSS border rules can be sized to the text, not the page.
  const int naturalLineWidth = lineWordWidthSum + naturalGapCount * spaceWidth;
  if (naturalLineWidth > static_cast<int>(maxLineContentWidth_)) {
    maxLineContentWidth_ = static_cast<uint16_t>(std::min(naturalLineWidth, 65535));
  }

  uint16_t currentIndent = 0;
  if (this->leftIndentLineCount > 0) {
    currentIndent = this->leftIndentWidth;
    this->leftIndentLineCount--;
  }

  const int effectivePageWidth = pageWidth - currentIndent;
  const int spareSpace = effectivePageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;
  const int gapCount = naturalGapCount;

  if (style == TextBlock::JUSTIFIED && !isLastLine && gapCount > 0) {
    if (spareSpace >= 0) {
      spacing = spareSpace / gapCount;
    } else {
      /** Greedy/DP mismatch or rounding: line is overfull — tighten gaps so words do not overlap the margin. */
      const int tightened = spaceWidth + spareSpace / gapCount;
      spacing = std::max(1, tightened);
    }
  }

  uint16_t xpos = currentIndent;
  if (style == TextBlock::RIGHT_ALIGN && spareSpace >= 0) {
    xpos += spareSpace - gapCount * spaceWidth;
  } else if (style == TextBlock::CENTER_ALIGN && spareSpace >= 0) {
    xpos += (spareSpace - gapCount * spaceWidth) / 2;
  }

  std::vector<uint16_t> lineXPos;
  int naturalGapIndex = 0;
  for (size_t i = lastBreakAt; i < lineBreak; i++) {
    const uint16_t currentWordWidth = wordWidths[i];
    lineXPos.push_back(xpos);
    int gapAfter = 0;
    if (i + 1 < lineBreak) {
      const bool nextJoinsThis = joinedAt(i + 1);
      gapAfter = nextJoinsThis ? 0 : spaceWidth;
      if (style == TextBlock::JUSTIFIED && !isLastLine && gapCount > 0) {
        if (nextJoinsThis) {
          gapAfter = 0;
        } else if (spareSpace >= 0) {
          const int rem = spareSpace % gapCount;
          gapAfter = spacing + (naturalGapIndex < rem ? 1 : 0);
        } else {
          gapAfter = spacing;
        }
      }
      if (!nextJoinsThis) {
        ++naturalGapIndex;
      }
    }
    xpos = static_cast<uint16_t>(static_cast<int>(xpos) + static_cast<int>(currentWordWidth) + gapAfter);
  }

  // A vector has no splice, so each line takes the first lineWordCount entries and the source
  // drops them. Moving the strings out first keeps this to one pass and no string copies; the
  // erase that follows is a memmove of what is left, which for a single line's worth of words is
  // cheaper than the per-node bookkeeping a list needed to avoid it.
  const auto takeFront = [lineWordCount](auto& source) {
    using Container = std::decay_t<decltype(source)>;
    const size_t count = std::min(lineWordCount, source.size());
    Container taken;
    taken.reserve(count);
    taken.insert(taken.end(), std::make_move_iterator(source.begin()),
                 std::make_move_iterator(source.begin() + static_cast<std::ptrdiff_t>(count)));
    source.erase(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(count));
    return taken;
  };

  auto lineWords = takeFront(words);
  auto lineWordStyles = takeFront(wordStyles);
  auto lineBionicPrefixBytes = takeFront(bionicPrefixBytes);
  auto lineWordSmallCaps = takeFront(wordSmallCaps);
  auto lineWordUnderline = takeFront(wordUnderline);
  auto lineWordVerticalAlign = takeFront(wordVerticalAlign);
  auto lineWordJoinPrevious = takeFront(wordJoinPrevious);

  // Image vectors are only populated when this block has inline images.
  std::vector<std::string> lineWordImagePaths;
  std::vector<uint16_t> lineWordImageW;
  std::vector<uint16_t> lineWordImageH;
  if (!wordImagePaths.empty()) {
    lineWordImagePaths = takeFront(wordImagePaths);
    lineWordImageW = takeFront(wordImageW);
    lineWordImageH = takeFront(wordImageH);
  }

  for (size_t i = 0; i < lineWords.size(); ++i) {
    if (!containsSoftHyphen(lineWords[i])) {
      continue;
    }
    stripSoftHyphensInPlace(lineWords[i]);
    if (bionicReadingEnabled && i < lineBionicPrefixBytes.size()) {
      lineBionicPrefixBytes[i] = bionicPrefixLengthBytes(lineWords[i]);
    }
  }

  processLine(std::make_shared<TextBlock>(
      std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), std::move(lineBionicPrefixBytes),
      std::move(lineWordSmallCaps), style, std::move(lineWordUnderline), std::move(lineWordVerticalAlign),
      std::move(lineWordImagePaths), std::move(lineWordImageW), std::move(lineWordImageH)));
}
