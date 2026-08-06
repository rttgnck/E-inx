#pragma once

/**
 * @file GfxRenderer.h
 * @brief Public interface and types for GfxRenderer.
 */

#include <EpdFontFamily.h>
#include <HalDisplay.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "../../src/system/ExternalFont.h"
#include "BitmapRender.h"
#include "IconRender.h"
#include "LineRender.h"
#include "PolygonRender.h"
#include "RectangleRender.h"
#include "TextRender.h"
#include "UiRender.h"

class GfxRenderer {
 public:
  // GRAY2_* : quality 2-bit planes used with the quality image LUT.
  enum RenderMode { BW, GRAYSCALE_LSB, GRAYSCALE_MSB, GRAY2_LSB, GRAY2_MSB };

  enum Orientation { Portrait, LandscapeClockwise, PortraitInverted, LandscapeCounterClockwise };

  enum ImageOrientation { None, Rotate90CW, Rotate180, Rotate270CW };

  enum class ReinforcementTarget : uint8_t { ReaderBw, MonochromeUi, DitheredThumbnail };

 private:
  static constexpr size_t BW_BUFFER_CHUNK_SIZE = 8000;

  HalDisplay& display;
  RenderMode renderMode;
  bool darkMode = false;
  bool fadingFix_ = false;
  bool preservingImageTone_ = false;
  mutable bool nextRefreshOverridePending_ = false;  ///< One-shot: force next displayBuffer() refresh mode.
  mutable HalDisplay::RefreshMode nextRefreshOverride_ = HalDisplay::FULL_REFRESH;
  mutable uint16_t x3ReinforcedRefreshCount_ = 0;
  mutable bool x3CleanupRequired_ = false;
  Orientation orientation;
  uint16_t panelWidth = HalDisplay::DISPLAY_WIDTH;
  uint16_t panelHeight = HalDisplay::DISPLAY_HEIGHT;
  uint16_t panelWidthBytes = HalDisplay::DISPLAY_WIDTH_BYTES;
  uint32_t frameBufferSize = HalDisplay::BUFFER_SIZE;
  std::vector<uint8_t*> bwBufferChunks;
  struct FontSlot {
    int id;
    EpdFontFamily family;
    FontSlot(int fontId, EpdFontFamily font) : id(fontId), family(std::move(font)) {}
  };
  struct StreamingFontSlot {
    const EpdFontData* data = nullptr;
    std::unique_ptr<ExternalFont> stream;
    StreamingFontSlot(const EpdFontData* fontData, std::unique_ptr<ExternalFont> fontStream)
        : data(fontData), stream(std::move(fontStream)) {}
  };
  std::vector<FontSlot> fontSlots;
  std::vector<StreamingFontSlot> streamingFontSlots;

  friend class BitmapRender;
  friend class TextRender;

  void freeBwBufferChunks();
  void rotateCoordinates(int x, int y, int* rotatedX, int* rotatedY) const;

 public:
  explicit GfxRenderer(HalDisplay& halDisplay);
  ~GfxRenderer();

  static constexpr int VIEWABLE_MARGIN_TOP = 9;
  static constexpr int VIEWABLE_MARGIN_RIGHT = 3;
  static constexpr int VIEWABLE_MARGIN_BOTTOM = 3;
  static constexpr int VIEWABLE_MARGIN_LEFT = 3;

  void insertFont(int fontId, EpdFontFamily font);
  void insertStreamingFont(int fontId, std::unique_ptr<ExternalFont> streamingFont, const EpdFontFamily& font);
  void removeFont(int fontId);
  void removeAllStreamingFonts();
  void addStreamingFontStyle(int fontId, EpdFontFamily::Style style, std::unique_ptr<ExternalFont> streamingFont);
  const EpdFontFamily* findFontFamily(int fontId) const;
  ExternalFont* findStreamingFont(const EpdFontData* data) const;

  void setOrientation(const Orientation o) { orientation = o; }
  Orientation getOrientation() const { return orientation; }

  int getScreenWidth() const;
  int getScreenHeight() const;
  void displayBuffer(const HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH) const;
  bool reinforcementEnabled(ReinforcementTarget target) const;
  void displayWithReinforcement(ReinforcementTarget target,
                                HalDisplay::RefreshMode fallback = HalDisplay::FAST_REFRESH) const;
  void allowReinforcementAfterTextAntiAliasing() const;
  void invertScreen() const;
  void clearScreen(uint8_t color = 0xFF) const;
  void begin();

  /**
   * @brief Forces the next displayBuffer() to use FULL_REFRESH regardless of the mode it is called with.
   * @details One-shot: consumed by the next push. Used when the whole screen's polarity flips (e.g. entering
   *          dark mode on boot/wake), where a partial/fast refresh can't cleanly re-develop the panel.
   */
  void requestNextRefresh(HalDisplay::RefreshMode mode) const {
    nextRefreshOverride_ = mode;
    nextRefreshOverridePending_ = true;
  }
  void requestNextFullRefresh() const { requestNextRefresh(HalDisplay::FULL_REFRESH); }
  void requestNextHalfRefresh() const { requestNextRefresh(HalDisplay::HALF_REFRESH); }

  /** Solid ink/paper, or Gray (50% checkerboard dither in BW, similar to light fills in list UIs). */
  enum class FillTone : uint8_t { Paper, Ink, Gray };

  void drawPixel(int x, int y, bool state = true) const;
  bool readPixel(int x, int y) const;
  bool readPackedRow1bpp(int x, int y, int width, uint8_t* outRow) const;
  void drawPackedRow1bpp(int x, int y, int width, const uint8_t* row) const;

  void drawImage(const uint8_t bitmap[], int x, int y, int width, int height,
                 ImageOrientation imgOrientation = None) const;

  /** Pixels outside the rounded clip after `Bitmap.Draw` (same geometry as rounded `fillRect`). */
  enum class BitmapRoundedCornerOutside : uint8_t {
    None = 0,
    PaperOutside = 1,
    /** ~25% ink on screen even/even pixels outside rounded corners (matches Recent carousel dither). */
    SparseInkAlignedOutside = 2,
  };

 private:
 public:
  void setRenderMode(const RenderMode mode) { this->renderMode = mode; }
  RenderMode getRenderMode() const { return renderMode; }

  /**
   * @brief Enables/disables global dark mode (system-wide BW color inversion).
   * @details When enabled, all 1-bit (BW) drawing is inverted at the pixel level so the whole
   *          interface renders light-on-dark. Grayscale image passes (renderMode != BW) are left
   *          untouched, so photos and covers keep their natural tones.
   */
  void setDarkMode(const bool enabled) { darkMode = enabled; }
  bool isDarkMode() const { return darkMode; }
  void setFadingFix(const bool enabled) { fadingFix_ = enabled; }
  bool isFadingFixEnabled() const { return fadingFix_; }
  bool setPreserveImageTone(const bool enabled) {
    const bool previous = preservingImageTone_;
    preservingImageTone_ = enabled;
    return previous;
  }
  bool deviceIsX3() const;
  void copyGrayscaleLsbBuffers() const;
  void copyGrayscaleMsbBuffers() const;
  void displayGrayBuffer(bool quality = false, bool trackForRevert = true) const;
  void displayGrayBufferFastQuality() const;
  void prepareQualityGrayscale() const;
  bool storeBwBuffer();
  void restoreBwBuffer();
  // Copies the stored BW shadow back into the framebuffer WITHOUT freeing it or touching controller RAM. Lets
  // the gray planes be rebuilt from the BW frame more than once (e.g. invert for LSB, then again for MSB).
  bool copyStoredBwToFramebuffer() const;
  void cleanupGrayscaleWithFrameBuffer() const;

  // One-call 2-bit grayscale sequence shared by sleep + reader. For each plane (LSB then MSB) it sets the
  // render mode, invokes drawPlane() to populate the framebuffer for that plane, and copies it into the
  // grayscale RAM bank; then it drives the gray refresh and resets to BW. `quality` selects GRAY2 + the
  // quality LUT, else GRAYSCALE + the fast LUT. When `preserveText` is true the BW baseline is restored from
  // the previously stored BW frame (call storeBwBuffer() first); otherwise it is rebased from a clean white
  // frame so the next BW refresh isn't polluted by the leftover grayscale plane.
  void renderGrayscalePasses(bool quality, bool preserveText, const std::function<void()>& drawPlane,
                             bool fastQuality = false);
  /** Drop BW shadow chunks, grayscale HAL state, and force BW mode (call when leaving image-heavy readers). */
  void resetTransientReaderState();

  uint8_t* getFrameBuffer() const;
  size_t getBufferSize() const;
  void grayscaleRevert() const;
  void getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const;

  RectangleRender rectangle;
  LineRender line;
  IconRender icon;
  PolygonRender polygon;
  BitmapRender bitmap;
  TextRender text;
  UiRender ui;
};
