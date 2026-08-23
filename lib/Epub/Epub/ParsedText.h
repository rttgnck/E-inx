#pragma once

/**
 * @file ParsedText.h
 * @brief Public interface and types for ParsedText.
 */

#include <EpdFontFamily.h>

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  // Vectors for the same reason TextBlock uses them: a list node spends two pointers and its own
  // heap allocation to carry one byte, and a chapter's worth of those fragments the heap badly
  // enough that a later image decode cannot find a contiguous block.
  std::vector<std::string> words;
  std::vector<EpdFontFamily::Style> wordStyles;
  std::vector<uint8_t> bionicPrefixBytes;
  std::vector<uint8_t> wordSmallCaps;
  std::vector<uint8_t> wordUnderline;
  std::vector<uint8_t> wordVerticalAlign;
  /** True when this token was split only by an inline style boundary and should not get an inter-word gap. */
  std::vector<uint8_t> wordJoinPrevious;
  // Inline images flow as atomic "words": for an image slot the `words` entry is empty and these hold the
  // cached path + on-line display size. These lists stay EMPTY (zero overhead) until the block actually
  // contains an inline image — see hasInlineImages_ — so plain text blocks pay nothing.
  std::vector<std::string> wordImagePaths;
  std::vector<uint16_t> wordImageW;
  std::vector<uint16_t> wordImageH;
  bool hasInlineImages_ = false;
  /** Widest natural (pre-alignment) line content width seen during layout; used to size CSS border rules. */
  uint16_t maxLineContentWidth_ = 0;
  TextBlock::Style style;
  bool extraParagraphSpacing;
  bool hyphenationEnabled;
  bool bionicReadingEnabled;
  /** Reader "Indent" / book setting: legacy first-line em and em-based CSS indent simulation. */
  bool respectParagraphIndent_ = true;
  /** Word-spacing multiplier (textSpace/100); scales the inter-word space used for layout. */
  float wordSpacingFactor_ = 1.0f;

  int cssTextIndentPx = -1;

  uint16_t leftIndentWidth = 0;
  uint16_t leftIndentLineCount = 0;
  void applyParagraphIndent(const GfxRenderer& renderer, int fontId);
  std::vector<size_t> computeLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth, int spaceWidth,
                                        std::vector<uint16_t>& wordWidths, int dropIndentW, int dropIndentLines);
  std::vector<size_t> computeHyphenatedLineBreaks(const GfxRenderer& renderer, int fontId, int pageWidth,
                                                  int spaceWidth, std::vector<uint16_t>& wordWidths, int dropIndentW,
                                                  int dropIndentLines);
  bool hyphenateWordAtIndex(size_t wordIndex, int availableWidth, const GfxRenderer& renderer, int fontId,
                            std::vector<uint16_t>& wordWidths, bool allowFallbackBreaks);
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<size_t>& lineBreakIndices, const std::vector<uint8_t>& joinPreviousSnapshot,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

  /**
   * Makes room for one more word across every parallel array, or reports that there is none.
   *
   * Reserving up front is what makes the appends that follow allocation-free, so a block that
   * cannot grow stops growing instead of throwing from the middle of a push and leaving the
   * arrays at different lengths. Before this existed, a failed append aborted the firmware and
   * rebooted the reader mid-book.
   */
  bool reserveForNextWord();

  /** Set once a block has refused a word, so the reason is logged once and not per word. */
  bool truncatedForMemory_ = false;

 public:
  explicit ParsedText(const TextBlock::Style style, const bool extraParagraphSpacing, const bool hyphenationEnabled,
                      const bool respectParagraphIndent = true, const bool bionicReadingEnabled = false,
                      const float wordSpacingFactor = 1.0f)
      : style(style),
        extraParagraphSpacing(extraParagraphSpacing),
        hyphenationEnabled(hyphenationEnabled),
        bionicReadingEnabled(bionicReadingEnabled),
        respectParagraphIndent_(respectParagraphIndent),
        wordSpacingFactor_(wordSpacingFactor) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle, bool smallCaps = false, bool underline = false,
               bool joinPrevious = false, uint8_t verticalAlign = TextBlock::BASELINE);
  /** Appends an inline image that flows on the line as an atomic word of the given on-screen size. */
  void addImage(std::string cachePath, uint16_t displayW, uint16_t displayH);
  void setStyle(const TextBlock::Style style) { this->style = style; }
  void setRespectParagraphIndent(bool v) { respectParagraphIndent_ = v; }
  void resetParagraphLayoutHints() {
    cssTextIndentPx = -1;
    leftIndentWidth = 0;
    leftIndentLineCount = 0;
  }

  void setLeftIndent(uint16_t width, uint16_t lineCount) {
    leftIndentWidth = width;
    leftIndentLineCount = lineCount;
  }

  /** Called when CSS declares text-indent (including 0); disables legacy first-line em otherwise used for left/justify.
   */
  void setCssTextIndentFromCascade(int resolvedPx) { cssTextIndentPx = (resolvedPx > 0) ? resolvedPx : 0; }

  TextBlock::Style getStyle() const { return style; }
  /** Widest natural line content width observed across all extracted lines of this block. */
  uint16_t maxLineContentWidth() const { return maxLineContentWidth_; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
};
