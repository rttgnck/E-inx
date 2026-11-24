/**
 * @file RecentActivity.cpp
 * @brief Definitions for RecentActivity.
 */

#include "RecentActivity.h"

#include <Bitmap.h>
#include <Epub/BookMetadataCache.h>
#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HardwareSerial.h>
#include <ImageRender.h>
#include <SDCardManager.h>
#include <Serialization.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <sstream>
#include <vector>

#include "../reader/Epub/EpubActivity.h"
#include "../reader/Epub/EpubAnnotations.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "components/recent/RecentLayouts.h"
#include "components/widget/Carousel.h"
#include "dictionary/DictionaryDefinitionLayout.h"
#include "images/Down.h"
#include "images/Star.h"
#include "images/Up.h"
#include "state/BookProgress.h"
#include "state/BookSetting.h"
#include "state/BookState.h"
#include "state/ReadingDailyStats.h"
#include "state/SavedDictionaryWords.h"
#include "state/Session.h"
#include "state/Statistics.h"
#include "state/SystemSetting.h"
#include "system/FontManager.h"
#include "system/Fonts.h"
#include "system/MappedInputManager.h"
#include "system/UiTheme.h"
#include "util/StringUtils.h"

extern bool sdCardAvailable;

namespace {

/**
 * Extracts the base filename from a full path.
 * Removes directory path and file extension, then replaces underscores with spaces.
 */
static std::string getBaseFilename(const std::string& filename) {
  size_t lastSlash = filename.find_last_of('/');
  std::string basename = (lastSlash != std::string::npos) ? filename.substr(lastSlash + 1) : filename;
  size_t lastDot = basename.find_last_of('.');
  if (lastDot != std::string::npos) {
    basename.resize(lastDot);
  }
  std::replace(basename.begin(), basename.end(), '_', ' ');
  return basename;
}

/**
 * Formats a title by capitalizing the first letter of each word.
 */
static std::string formatTitle(const std::string& title) {
  std::string formatted = title;
  bool capitalizeNext = true;
  for (char& c : formatted) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      capitalizeNext = true;
    } else if (capitalizeNext) {
      c = std::toupper(static_cast<unsigned char>(c));
      capitalizeNext = false;
    }
  }
  return formatted;
}

static std::string bookDisplayTitle(const RecentBook& book) {
  if (!book.title.empty()) {
    return book.title;
  }
  return formatTitle(getBaseFilename(book.path));
}

static std::string formatDailyDuration(const uint32_t ms) {
  const uint32_t minutes = ms / 60000UL;
  const uint32_t hours = minutes / 60UL;
  char buf[24];
  if (hours > 0) {
    snprintf(buf, sizeof(buf), "%uh %02um", static_cast<unsigned>(hours), static_cast<unsigned>(minutes % 60UL));
  } else {
    snprintf(buf, sizeof(buf), "%um", static_cast<unsigned>(minutes));
  }
  return std::string(buf);
}

static std::string formatDailyDurationCompact(const uint32_t ms, const bool omitZeroMinuteSuffix = false) {
  const uint32_t minutes = ms / 60000UL;
  const uint32_t hours = minutes / 60UL;
  char buf[20];
  if (hours > 0) {
    snprintf(buf, sizeof(buf), "%uh%02u", static_cast<unsigned>(hours), static_cast<unsigned>(minutes % 60UL));
  } else if (minutes == 0 && omitZeroMinuteSuffix) {
    snprintf(buf, sizeof(buf), "0");
  } else {
    snprintf(buf, sizeof(buf), "%um", static_cast<unsigned>(minutes));
  }
  return std::string(buf);
}

static bool recentBookFinished(const RecentBook& book) {
  if (book.progress >= 0.995f) {
    return true;
  }
  BookState::Book state;
  return BOOK_STATE.findBook(book.path, state) && state.isFinished;
}

static float recentDisplayProgress(const RecentBook& book) {
  return recentBookFinished(book) ? 1.0f : book.progress;
}

constexpr unsigned long GO_HOME_MS = 1000;

/** O(1): bounded switch on RECENT_LIBRARY_MODE (fixed enum cardinality). */
static RecentActivity::ViewMode viewModeForLibrarySetting(uint8_t mode) {
  using SM = SystemSetting;
  switch (mode) {
    case SM::RECENT_GRID:
      return RecentActivity::ViewMode::Grid;
    case SM::RECENT_LIST_DEPRECATED:
      return RecentActivity::ViewMode::Flow;
    case SM::RECENT_STATS:
      return RecentActivity::ViewMode::StatsDashboard;
    case SM::RECENT_SIMPLE:
      return RecentActivity::ViewMode::Flow;
    case SM::RECENT_BOOK_LIST:
      return RecentActivity::ViewMode::List;
    case SM::RECENT_ICONS:
      return RecentActivity::ViewMode::Icons;
    case SM::RECENT_COVER:
      return RecentActivity::ViewMode::Cover;
    case SM::RECENT_FLOW:
    default:
      return RecentActivity::ViewMode::Flow;
  }
}

/** No-cover: stats-style double frame + title one word per line, each line centered (see
 * StatisticActivity::renderCover). */
static void drawRecentNoCoverPlaceholder(GfxRenderer& renderer, int x, int y, int w, int h, const std::string& title,
                                         int fontId) {
  if (w <= 1 || h <= 1) {
    return;
  }
  const bool rr = SETTINGS.bitmapRoundedCorners != 0;
  const bool subtle = SETTINGS.bitmapRoundedCorners == 2;
  renderer.rectangle.fill(x, y, w, h, false, rr, subtle);
  if (!rr) {
    renderer.rectangle.render(x, y, w, h, true, false);
    if (w > 6 && h > 6) {
      renderer.rectangle.render(x + 2, y + 2, w - 4, h - 4, true, false);
    }
  }

  const int lh = renderer.text.getLineHeight(fontId);
  const int maxLines = std::max(1, (h - 12) / std::max(1, lh));
  int wordCount = 0;
  bool inWord = false;
  for (char c : title) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      inWord = false;
    } else if (!inWord) {
      inWord = true;
      ++wordCount;
    }
  }
  if (wordCount <= 0) {
    return;
  }
  const int linesToDraw = std::min(maxLines, wordCount);
  const int totalTextH = linesToDraw * lh;
  int lineY = y + std::max(4, (h - totalTextH) / 2);
  const int innerPad = 6;
  const int maxWordW = std::max(8, w - 2 * innerPad);

  size_t pos = 0;
  for (int line = 0; line < linesToDraw; ++line) {
    while (pos < title.size() && std::isspace(static_cast<unsigned char>(title[pos]))) {
      ++pos;
    }
    size_t end = pos;
    while (end < title.size() && !std::isspace(static_cast<unsigned char>(title[end]))) {
      ++end;
    }
    std::string wshow = title.substr(pos, end - pos);
    if (renderer.text.getWidth(fontId, wshow.c_str()) > maxWordW) {
      wshow = renderer.text.truncate(fontId, wshow.c_str(), maxWordW, EpdFontFamily::REGULAR);
    }
    const int tw = renderer.text.getWidth(fontId, wshow.c_str());
    renderer.text.render(fontId, x + (w - tw) / 2, lineY, wshow.c_str(), true, EpdFontFamily::REGULAR);
    lineY += lh;
    pos = end;
  }
}

static std::string epubCachePathForBookPath(const std::string& bookPath) {
  return "/.metadata/epub/" + std::to_string(std::hash<std::string>{}(bookPath));
}

struct IconRect {
  int x;
  int y;
  int w;
  int h;
};

static IconRect fitBookCoverRect(int x, int y, int w, int h) {
  int coverW = w;
  int coverH = coverW * RecentActivity::COVER_HEIGHT / RecentActivity::COVER_WIDTH;
  if (coverH > h) {
    coverH = h;
    coverW = coverH * RecentActivity::COVER_WIDTH / RecentActivity::COVER_HEIGHT;
  }
  return {x + (w - coverW) / 2, y + (h - coverH) / 2, coverW, coverH};
}

static IconRect inflateIconRect(const IconRect& rect, int percent) {
  const int extraW = std::max(2, rect.w * percent / 100);
  const int extraH = std::max(2, rect.h * percent / 100);
  return {rect.x - extraW / 2, rect.y - extraH / 2, rect.w + extraW, rect.h + extraH};
}

static void renderThickIconRect(const GfxRenderer& renderer, const IconRect& rect, bool rounded, int thickness) {
  for (int i = 0; i < thickness; ++i) {
    renderer.rectangle.render(rect.x - i, rect.y - i, rect.w + i * 2, rect.h + i * 2, true, rounded);
  }
}

static void drawRecentThumbnailPlaceholder(GfxRenderer& renderer, const int x, const int y, const int w, const int h,
                                           const bool selected = false) {
  if (w <= 0 || h <= 0) {
    return;
  }
  const bool rounded = SETTINGS.bitmapRoundedCorners != 0;
  renderer.rectangle.fill(x, y, w, h, false, rounded);
  renderer.rectangle.render(x, y, w, h, true, rounded);
  if (selected) {
    renderer.rectangle.render(x - 2, y - 2, w + 4, h + 4, true, rounded);
  }
}

/** Flow-style dither strictly inside a list row (does not bleed into adjacent rows). */
static void drawFlowCarouselBackdropInRect(const GfxRenderer& renderer, const int rx, const int ry, const int rw,
                                           const int rh) {
  if (rw <= 0 || rh <= 0) {
    return;
  }
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int x1 = std::max(0, rx);
  const int y1 = std::max(0, ry);
  const int x2 = std::min(screenW, rx + rw);
  const int y2 = std::min(screenH, ry + rh);
  for (int y = (y1 + 1) & ~1; y < y2; y += 2) {
    for (int x = (x1 + 1) & ~1; x < x2; x += 2) {
      renderer.drawPixel(x, y, true);
    }
  }
}

static void drawProgressBadge(const GfxRenderer& renderer, const IconRect& coverRect, float progress) {
  if (progress < 0.0f || progress > 1.0f) {
    return;
  }
  char label[12];
  if (progress >= 0.995f) {
    snprintf(label, sizeof(label), "100%%");
  } else {
    snprintf(label, sizeof(label), "%d%%", static_cast<int>(progress * 100.0f + 0.5f));
  }
  constexpr int font = MONTSERRAT_8_FONT_ID;
  const int textW = renderer.text.getWidth(font, label);
  const int textH = renderer.text.getLineHeight(font);
  const int badgeW = std::max(22, textW + 8);
  const int badgeH = textH + 4;
  const int badgeX = coverRect.x + coverRect.w - badgeW - 2;
  const int badgeY = coverRect.y + 2;
  renderer.rectangle.fill(badgeX, badgeY, badgeW, badgeH, true);
  renderer.text.render(font, badgeX + (badgeW - textW) / 2, badgeY + 1, label, false, EpdFontFamily::BOLD);
}

}  // namespace

namespace {
constexpr int kRecentThumbGap = 20;
constexpr int kRecentStripSlots = 2;

/** Keep horizontal strip window so `selectorIndex` is one of the two visible slots. */
inline void clampRecentStripHScroll(int sel, int bookCount, int& hScroll) {
  if (bookCount <= 0) {
    return;
  }
  const int maxH = std::max(0, bookCount - kRecentStripSlots);
  int h = std::min(maxH, sel);
  h = std::max(0, std::max(h, sel - (kRecentStripSlots - 1)));
  hScroll = h;
}

}  // namespace

#include "components/recent/HomeMenuDrawer.ipp"

RecentActivity::~RecentActivity() {
  delete homeMenuDrawer_;
  homeMenuDrawer_ = nullptr;
  freeRecentPageBuffer();
}

void RecentActivity::drawRecentThumbnailAt(int x, int y, int w, int h, const std::string& cacheDir,
                                           const std::string& placeholderTitle, int placeholderFontId,
                                           const bool roundedCornerBackdropIsDither) {
  if (w < 8 || h < 8) {
    return;
  }
  if (cacheDir.empty()) {
    drawRecentNoCoverPlaceholder(renderer, x, y, w, h, placeholderTitle, placeholderFontId);
    return;
  }
  const std::string imagePath = resolveThumbnailPath(cacheDir);

  if (!imagePath.empty()) {
    ImageRender::Options options;
    options.cropToFill = true;
    options.useDisplayCache = true;
    if (SETTINGS.bitmapRoundedCorners == 0) {
      options.roundedOutside = BitmapRender::RoundedOutside::None;
    } else if (SETTINGS.bitmapRoundedCorners == 2) {
      options.roundedOutside = roundedCornerBackdropIsDither
                                   ? BitmapRender::RoundedOutside::SubtleSparseInkAlignedOutside
                                   : BitmapRender::RoundedOutside::SubtlePaperOutside;
    } else {
      options.roundedOutside = roundedCornerBackdropIsDither ? BitmapRender::RoundedOutside::SparseInkAlignedOutside
                                                             : BitmapRender::RoundedOutside::PaperOutside;
    }

    const ImageRender image = ImageRender::create(renderer, imagePath);
    if (image.renderDisplayCacheOnly(x, y, w, h, options)) {
      return;
    }
    if (queueRecentImageCacheBuild(imagePath, x, y, w, h, options.cropToFill, options.roundedOutside)) {
      drawRecentThumbnailPlaceholder(renderer, x, y, w, h);
      return;
    }
    renderer.rectangle.fill(x, y, w, h, false);
    if (image.render(x, y, w, h, options)) {
      return;
    }
  }

  drawRecentNoCoverPlaceholder(renderer, x, y, w, h, placeholderTitle, placeholderFontId);
}

void RecentActivity::renderFlowThumbnail(void* context, const RecentBook& book, const int x, const int y, const int w,
                                         const int h, const int placeholderFontId,
                                         const bool roundedCornerBackdropIsDither) {
  if (context == nullptr) {
    return;
  }
  auto* self = static_cast<RecentActivity*>(context);
  self->drawRecentThumbnailAt(x, y, w, h, book.cachePath, bookDisplayTitle(book), placeholderFontId,
                              roundedCornerBackdropIsDither);
}

void RecentActivity::drawRecentCoverFitAt(int x, int y, int w, int h, const std::string& cacheDir,
                                          const std::string& placeholderTitle, int placeholderFontId) {
  if (w < 8 || h < 8) {
    return;
  }
  if (cacheDir.empty()) {
    drawRecentNoCoverPlaceholder(renderer, x, y, w, h, placeholderTitle, placeholderFontId);
    return;
  }

  const std::string imagePath = resolveCoverPath(cacheDir);
  if (!imagePath.empty()) {
    ImageRender::Options options;
    options.cropToFill = false;
    options.useDisplayCache = true;
    if (SETTINGS.bitmapRoundedCorners == 0) {
      options.roundedOutside = BitmapRender::RoundedOutside::None;
    } else if (SETTINGS.bitmapRoundedCorners == 2) {
      options.roundedOutside = BitmapRender::RoundedOutside::SubtlePaperOutside;
    } else {
      options.roundedOutside = BitmapRender::RoundedOutside::PaperOutside;
    }

    const ImageRender image = ImageRender::create(renderer, imagePath);
    if (image.renderDisplayCacheOnly(x, y, w, h, options)) {
      return;
    }
    if (queueRecentImageCacheBuild(imagePath, x, y, w, h, options.cropToFill, options.roundedOutside)) {
      drawRecentThumbnailPlaceholder(renderer, x, y, w, h);
      return;
    }
    renderer.rectangle.fill(x, y, w, h, false, SETTINGS.bitmapRoundedCorners != 0);
    if (image.render(x, y, w, h, options)) {
      return;
    }
  }

  drawRecentNoCoverPlaceholder(renderer, x, y, w, h, placeholderTitle, placeholderFontId);
}

std::string RecentActivity::resolveThumbnailPath(const std::string& cacheDir) const {
  if (cacheDir.empty()) {
    return "";
  }
  const auto cached = thumbnailPathCache_.find(cacheDir);
  if (cached != thumbnailPathCache_.end()) {
    return cached->second;
  }

  char jpegPath[192];
  snprintf(jpegPath, sizeof(jpegPath), "%s/thumb.jpg", cacheDir.c_str());
  if (SdMan.exists(jpegPath)) {
    thumbnailPathCache_[cacheDir] = jpegPath;
    return jpegPath;
  }

  char pngPath[192];
  snprintf(pngPath, sizeof(pngPath), "%s/thumb.png", cacheDir.c_str());
  if (SdMan.exists(pngPath)) {
    thumbnailPathCache_[cacheDir] = pngPath;
    return pngPath;
  }

  char bmpPath[192];
  snprintf(bmpPath, sizeof(bmpPath), "%s/thumb.bmp", cacheDir.c_str());
  if (SdMan.exists(bmpPath)) {
    thumbnailPathCache_[cacheDir] = bmpPath;
    return bmpPath;
  }

  thumbnailPathCache_[cacheDir] = "";
  return "";
}

std::string RecentActivity::resolveCoverPath(const std::string& cacheDir) const {
  if (cacheDir.empty()) {
    return "";
  }
  const auto cached = coverPathCache_.find(cacheDir);
  if (cached != coverPathCache_.end()) {
    return cached->second;
  }

  const char* names[] = {"cover.jpg", "cover.png", "cover.bmp", "cover_crop.jpg", "cover_crop.bmp"};
  for (const char* name : names) {
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", cacheDir.c_str(), name);
    if (SdMan.exists(path)) {
      coverPathCache_[cacheDir] = path;
      return path;
    }
  }

  const std::string fallback = resolveThumbnailPath(cacheDir);
  coverPathCache_[cacheDir] = fallback;
  return fallback;
}

/**
 * Calculates the number of rows that can be displayed on screen at once.
 */
int RecentActivity::getVisibleRows() const {
  if (currentViewMode == ViewMode::Icons) {
    return ICON_ROWS;
  }
  if (currentViewMode == ViewMode::Grid) {
    int availableHeight = INX_THEME.mainContentBottom(renderer) - recentGridPaintStartY() - 20;
    return (availableHeight > 0) ? availableHeight / GRID_ITEM_HEIGHT : 1;
  }
  if (currentViewMode == ViewMode::List) {
    return LIST_VISIBLE_ITEMS;
  }
  return LIST_VISIBLE_ITEMS;
}

/**
 * Loads recent books from persistent storage.
 */
void RecentActivity::loadRecentBooks(const bool resetScroll) {
  freeRecentPageBuffer();
  recentBooks.clear();
  recentStats_.clear();
  const int maxShow = MAX_RECENT_BOOKS;
  if (resetScroll) {
    scrollOffset = 0;
  }

  const auto& allBooks = RECENT_BOOKS.getBooks();
  size_t addedCount = 0;

  for (size_t i = 0; i < allBooks.size() && addedCount < maxShow; ++i) {
    const auto& book = allBooks[i];
    recentBooks.push_back(book);
    recentStats_.push_back(CachedRecentStats{});
    addedCount++;
  }
}

bool RecentActivity::openBookPath(const std::string& path, const std::string& title, const std::string& author,
                                  const bool removeMissingFromRecents, const bool openNavigation) {
  if (path.empty()) {
    return false;
  }
  const std::string selectedPath = path;

  // SD access can transiently fail under contention (this page streams thumbnails/stats), making a present
  // book read as "missing". Retry a few times before treating it as gone, so a valid book always opens
  // instead of being removed from recents and resetting the selection to the first book.
  bool present = SdMan.exists(selectedPath.c_str());
  for (int attempt = 0; !present && attempt < 4; ++attempt) {
    delay(25);
    present = SdMan.exists(selectedPath.c_str());
  }

  if (!present) {
    if (removeMissingFromRecents) {
      RECENT_BOOKS.removeBook(selectedPath);
      loadRecentBooks(false);
      const int n = static_cast<int>(recentBooks.size());
      selectorIndex = n == 0 ? 0 : std::min(selectorIndex, n - 1);
      scrollOffset = 0;
      updateRequired = true;
    }
    return false;
  }

  bookSelected = true;
  if (openNavigation && onSelectBookNavigation) {
    onSelectBookNavigation(selectedPath);
  } else {
    onSelectBook(selectedPath);
  }
  return true;
}

int RecentActivity::selectedRecentIndexForRemove() const {
  if (recentBooks.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(recentBooks.size())) {
    return -1;
  }
  return selectorIndex;
}

void RecentActivity::beginRemoveConfirmation() {
  const int index = selectedRecentIndexForRemove();
  if (index < 0) {
    return;
  }
  removeConfirmIndex_ = index;
  removeConfirmOpen_ = true;
  freeRecentPageBuffer();
  updateRequired = true;
}

void RecentActivity::cancelRemoveConfirmation() {
  removeConfirmOpen_ = false;
  removeConfirmIndex_ = -1;
  freeRecentPageBuffer();
  updateRequired = true;
}

void RecentActivity::confirmRemoveRecent() {
  if (removeConfirmIndex_ >= 0 && removeConfirmIndex_ < static_cast<int>(recentBooks.size())) {
    RECENT_BOOKS.removeBook(recentBooks[static_cast<size_t>(removeConfirmIndex_)].path);
  }

  removeConfirmOpen_ = false;
  removeConfirmIndex_ = -1;
  loadRecentBooks(false);

  const int n = static_cast<int>(recentBooks.size());
  if (n == 0) {
    selectorIndex = 0;
    scrollOffset = 0;
  } else {
    if (selectorIndex >= n) {
      selectorIndex = n - 1;
    }
    if (currentViewMode == ViewMode::Grid || currentViewMode == ViewMode::Icons) {
      const int cols = currentViewMode == ViewMode::Icons ? ICON_COLS : GRID_COLS;
      const int visibleRows = getVisibleRows();
      const int totalRows = (n + cols - 1) / cols;
      const int maxScroll = std::max(0, totalRows - visibleRows);
      scrollOffset = std::max(0, std::min(scrollOffset, maxScroll));
    } else if (currentViewMode == ViewMode::List) {
      const int maxScroll = std::max(0, n - LIST_VISIBLE_ITEMS);
      scrollOffset = std::max(0, std::min(scrollOffset, maxScroll));
    }
  }
  freeRecentPageBuffer();
  updateRequired = true;
}

void RecentActivity::renderRemoveConfirmation() {
  renderer.clearScreen();
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int centerY = screenH / 2;

  std::string title = "this book";
  if (removeConfirmIndex_ >= 0 && removeConfirmIndex_ < static_cast<int>(recentBooks.size())) {
    title = bookDisplayTitle(recentBooks[static_cast<size_t>(removeConfirmIndex_)]);
  }
  title = renderer.text.truncate(MONTSERRAT_10_FONT_ID, title.c_str(), screenW - 64);

  renderer.text.centered(MONTSERRAT_8_FONT_ID, centerY - 92, "RECENT BOOK", true, EpdFontFamily::BOLD);
  renderer.text.centered(MONTSERRAT_14_FONT_ID, centerY - 54, "Remove from recent?", true,
                         EpdFontFamily::BOLD);
  renderer.text.centered(MONTSERRAT_10_FONT_ID, centerY - 10, title.c_str(), true, EpdFontFamily::REGULAR);
  renderer.text.centered(MONTSERRAT_8_FONT_ID, centerY + 26, "The book file and reading progress will stay.",
                         true, EpdFontFamily::REGULAR);
  const auto labels = mappedInput.mapLabels("Cancel", "Remove", "", "");
  renderer.ui.buttonHints(ATKINSON_HYPERLEGIBLE_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayWithReinforcement(GfxRenderer::ReinforcementTarget::MonochromeUi);
  updateRequired = false;
}

const RecentActivity::CachedRecentStats& RecentActivity::statsForRecentIndex(const int index) const {
  static const CachedRecentStats empty;
  if (index < 0 || index >= static_cast<int>(recentStats_.size()) || index >= static_cast<int>(recentBooks.size())) {
    return empty;
  }
  CachedRecentStats& cached = recentStats_[static_cast<size_t>(index)];
  if (!cached.attempted) {
    cached.attempted = true;
    const RecentBook& book = recentBooks[static_cast<size_t>(index)];
    std::string cachePath = book.cachePath;
    if (cachePath.empty()) {
      cachePath = epubCachePathForBookPath(book.path);
    }
    cached.loaded = !cachePath.empty() && loadBookStats(cachePath.c_str(), cached.stats);
  }
  return cached;
}

/**
 * Initializes the recent activity when entered.
 */
void RecentActivity::onEnter() {
  Activity::onEnter();

  freeRecentPageBuffer();
  layoutEngine_.reset();
  layoutEngineBoundMode_ = ViewMode::Flow;
  halfRefreshOnLoadApplied_ = false;
  removeConfirmOpen_ = false;
  removeConfirmIndex_ = -1;
  ignoreBackReleaseOnEnter_ = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Back);
  renderer.clearScreen(0xff);
  loadRecentBooks();

  currentViewMode = viewModeForLibrarySetting(SETTINGS.recentLibraryMode);
  if (currentViewMode == ViewMode::List || currentViewMode == ViewMode::Icons ||
      currentViewMode == ViewMode::StatsDashboard) {
    selectorIndex = 0;
    scrollOffset = 0;
  }

  pendingInitialLoadingFrame_ = false;
  updateRequired = true;
}

/**
 * Cleans up resources when exiting the recent activity.
 */
void RecentActivity::onExit() {
  freeRecentPageBuffer();
  resetRecentImageCacheJobs();
  removeConfirmOpen_ = false;
  removeConfirmIndex_ = -1;
  delete homeMenuDrawer_;
  homeMenuDrawer_ = nullptr;
  layoutEngine_.reset();
  layoutEngineBoundMode_ = ViewMode::Flow;
  std::vector<RecentBook>().swap(recentBooks);
  std::vector<CachedRecentStats>().swap(recentStats_);
  std::unordered_map<std::string, std::string>().swap(thumbnailPathCache_);
  std::unordered_map<std::string, std::string>().swap(coverPathCache_);
  renderer.resetTransientReaderState();
  Activity::onExit();
}

void RecentActivity::renderInitialLoadingFrame() {
  renderer.clearScreen();
  renderTabBar(renderer);

  const int top = mainContentTop();
  const int bottom = INX_THEME.mainTabsAtBottom() ? mainContentBottom(renderer) : renderer.getScreenHeight() - 42;
  const int centerY = top + std::max(1, bottom - top) / 2 - 12;
  renderer.text.centered(MONTSERRAT_10_FONT_ID, centerY, "Loading recents");

  renderer.displayWithReinforcement(GfxRenderer::ReinforcementTarget::MonochromeUi);
}

void RecentActivity::renderSdCardUnavailableMessage() {
  const int top = mainContentTop();
  const int bottom = INX_THEME.mainTabsAtBottom() ? mainContentBottom(renderer) : renderer.getScreenHeight() - 42;
  const int lineHeight = renderer.text.getLineHeight(MONTSERRAT_10_FONT_ID);
  const int centerY = top + std::max(1, bottom - top) / 2;

  renderer.text.centered(MONTSERRAT_10_FONT_ID, centerY - lineHeight, "SD card not available", true,
                         EpdFontFamily::BOLD);
  renderer.text.centered(MONTSERRAT_8_FONT_ID, centerY + 8, "Storage features are disabled");
}

void RecentActivity::openHomeMenuDrawer() {
  freeRecentPageBuffer();
  if (!homeMenuDrawer_) {
    homeMenuDrawer_ = new HomeMenuDrawer(*this);
  }
  homeMenuDrawer_->show();
}

void RecentActivity::closeHomeMenuDrawer() {
  if (homeMenuDrawer_) {
    homeMenuDrawer_->hide();
  }
}

#include "components/recent/Cover.ipp"
#include "components/recent/Flow.ipp"
#include "components/recent/Grid.ipp"
#include "components/recent/List.ipp"

void RecentActivity::drawBufferedSelectionOverlay() {
  const int totalBooks = static_cast<int>(recentBooks.size());
  if (selectorIndex < 0 || selectorIndex >= totalBooks) {
    return;
  }

  if (currentViewMode == ViewMode::Grid) {
    const int selectedRow = selectorIndex / GRID_COLS;
    const int visibleRows = getVisibleRows();
    if (selectedRow < scrollOffset || selectedRow >= scrollOffset + visibleRows) {
      return;
    }
    renderGridItem(selectorIndex % GRID_COLS, selectedRow - scrollOffset, recentGridPaintStartY(),
                   recentBooks[static_cast<size_t>(selectorIndex)], true);
    return;
  }

  if (currentViewMode == ViewMode::List) {
    if (selectorIndex < scrollOffset || selectorIndex >= scrollOffset + LIST_VISIBLE_ITEMS) {
      return;
    }
    renderList(recentListPaintStartY());
    return;
  }

  if (currentViewMode != ViewMode::Icons) {
    return;
  }

  constexpr int kCols = ICON_COLS;
  constexpr int kRowsVisible = ICON_ROWS;
  constexpr int kGap = 8;
  constexpr int kMarginX = 10;
  constexpr int kMarginY = 8;
  const int selectedRow = selectorIndex / kCols;
  if (selectedRow < scrollOffset || selectedRow >= scrollOffset + kRowsVisible) {
    return;
  }

  const int startY = recentIconsPaintStartY();
  const int screenW = renderer.getScreenWidth();
  const int contentBottom =
      INX_THEME.mainTabsAtBottom() ? mainContentBottom(renderer) : renderer.getScreenHeight() - 54;
  const int availW = std::max(1, screenW - kMarginX * 2);
  const int availH = std::max(1, contentBottom - startY - kMarginY * 2);
  const int frameW = std::max(40, (availW - (kCols - 1) * kGap) / kCols);
  const int frameH = std::max(40, (availH - (kRowsVisible - 1) * kGap) / kRowsVisible);
  const int blockW = kCols * frameW + (kCols - 1) * kGap;
  const int blockH = kRowsVisible * frameH + (kRowsVisible - 1) * kGap;
  const int row0X = kMarginX + std::max(0, (availW - blockW) / 2);
  const int blockTop = startY + kMarginY + std::max(0, (availH - blockH) / 2);
  const int col = selectorIndex % kCols;
  const int visualRow = selectedRow - scrollOffset;
  const int boxX = row0X + col * (frameW + kGap);
  const int boxY = blockTop + visualRow * (frameH + kGap);
  constexpr int kInnerPad = 4;
  const int innerX = boxX + kInnerPad;
  const int innerY = boxY + kInnerPad;
  const int innerW = std::max(8, frameW - kInnerPad * 2);
  const int innerH = std::max(8, frameH - kInnerPad * 2);
  const IconRect coverRect = inflateIconRect(fitBookCoverRect(innerX, innerY, innerW, innerH), 5);
  renderThickIconRect(renderer, coverRect, SETTINGS.bitmapRoundedCorners != 0, 3);
}

/**
 * Renders a single grid item including cover image or placeholder.
 */
void RecentActivity::renderGridItem(int gridX, int gridY, int startY, const RecentBook& book, bool selected) {
  const int screenW = renderer.getScreenWidth();
  const int contentBottom =
      INX_THEME.mainTabsAtBottom() ? mainContentBottom(renderer) : renderer.getScreenHeight() - 54;
  constexpr int kGridSpacing = 8;

  int availableWidth = screenW - (GRID_COLS + 1) * kGridSpacing;
  int containerWidth = availableWidth / GRID_COLS;

  int visibleRows = getVisibleRows();
  int availableHeight = contentBottom - startY - (kGridSpacing * 2);
  int containerHeight = std::max(1, (availableHeight / visibleRows) - kGridSpacing);

  int itemX = kGridSpacing + gridX * (containerWidth + kGridSpacing);
  int itemY = startY + kGridSpacing + gridY * (containerHeight + kGridSpacing);

  constexpr int kGridThumbnailWidthScalePercent = 95;
  const int drawW = std::max(1, containerWidth * kGridThumbnailWidthScalePercent / 100);
  const int drawH = containerHeight;
  const int drawX = itemX + (containerWidth - drawW) / 2;
  const int drawY = itemY + (containerHeight - drawH) / 2;

  if (selected) {
    const int overlayX = std::max(0, drawX - 8);
    const int overlayY = std::max(startY, drawY - 8);
    const int overlayW = std::min(screenW - overlayX, drawW + 16);
    const int overlayH = std::min(contentBottom - overlayY, drawH + 16);
    for (int y = overlayY; y < overlayY + overlayH; y += 2) {
      for (int x = overlayX; x < overlayX + overlayW; x += 2) {
        renderer.drawPixel(x, y, true);
      }
    }
  }
  drawRecentThumbnailAt(drawX, drawY, drawW, drawH, book.cachePath, bookDisplayTitle(book),
                        MONTSERRAT_10_FONT_ID, selected);

  if (book.progress >= 0.0f && book.progress <= 1.0f) {
    const float prog = recentDisplayProgress(book);
    int barX = drawX + 15;
    constexpr int kGridProgressBottomInset = 16;
    int barY = drawY + drawH - kGridProgressBottomInset;
    int barW = drawW - 30;
    int barH = 6;
    char pText[12];
    if (recentBookFinished(book)) {
      snprintf(pText, sizeof(pText), "Finished");
    } else {
      snprintf(pText, sizeof(pText), "%d%%", static_cast<int>(prog * 100.0f + 0.5f));
    }
    constexpr int kPctFont = MONTSERRAT_8_FONT_ID;
    const int pW = renderer.text.getWidth(kPctFont, pText);
    const int pH = renderer.text.getLineHeight(kPctFont);
    const int pX = barX + barW - pW;
    const int pY = barY - pH - 4;

    renderer.rectangle.fill(barX - 2, barY - 2, barW + 4, barH + 4, false, true);

    renderer.rectangle.fill(barX, barY, barW, barH, false);
    renderer.rectangle.render(barX, barY, barW, barH, true);

    if (prog > 0.0f) {
      int fillW = static_cast<int>(barW * prog + 0.5f);
      renderer.rectangle.fill(barX, barY, fillW, barH);
    }
    renderer.text.render(kPctFont, pX, pY, pText);
  }
}

void RecentActivity::syncLayoutEngineForViewMode() {
  if (!layoutEngine_ || layoutEngineBoundMode_ != currentViewMode) {
    layoutEngine_ = makeLayoutEngine(currentViewMode);
    layoutEngineBoundMode_ = currentViewMode;
  }
}

std::unique_ptr<RecentActivity::LayoutEngine> RecentActivity::makeLayoutEngine(ViewMode mode) {
  switch (mode) {
    case ViewMode::Grid:
      return std::unique_ptr<LayoutEngine>(new GridViewLayout());
    case ViewMode::Icons:
      return std::unique_ptr<LayoutEngine>(new IconsViewLayout());
    case ViewMode::Cover:
      return std::unique_ptr<LayoutEngine>(new CoverViewLayout());
    case ViewMode::List:
      return std::unique_ptr<LayoutEngine>(new ListViewLayout());
    case ViewMode::StatsDashboard:
      return std::unique_ptr<LayoutEngine>(new StatsDashboardViewLayout());
    case ViewMode::Flow:
    default:
      return std::unique_ptr<LayoutEngine>(new FlowViewLayout());
  }
}

void RecentActivity::GridViewLayout::paint(RecentActivity& self) {
  recent::Grid::render(self, self.recentGridPaintStartY());
}

void RecentActivity::IconsViewLayout::paint(RecentActivity& self) {
  recent::Grid3x3::render(self, self.recentIconsPaintStartY());
}

void RecentActivity::CoverViewLayout::paint(RecentActivity& self) { recent::Cover::render(self); }

void RecentActivity::ListViewLayout::paint(RecentActivity& self) {
  recent::List::render(self, self.recentListPaintStartY());
}

void RecentActivity::FlowViewLayout::paint(RecentActivity& self) { recent::Flow::render(self); }

void RecentActivity::StatsDashboardViewLayout::paint(RecentActivity& self) { self.renderStatsDashboard(); }

bool RecentActivity::canUseRecentPageBuffer() const {
  return !recentBooks.empty() &&
         (currentViewMode == ViewMode::Grid || currentViewMode == ViewMode::Icons || currentViewMode == ViewMode::List);
}

bool RecentActivity::storeRecentPageBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  freeRecentPageBuffer();

  const size_t bufferSize = renderer.getBufferSize();
  recentPageBuffer_ = static_cast<uint8_t*>(malloc(bufferSize));
  if (!recentPageBuffer_) {
    return false;
  }

  memcpy(recentPageBuffer_, frameBuffer, bufferSize);
  recentPageBufferStored_ = true;
  recentPageBufferMode_ = currentViewMode;
  recentPageBufferScrollOffset_ = scrollOffset;
  recentPageBufferBookCount_ = static_cast<int>(recentBooks.size());
  return true;
}

bool RecentActivity::restoreRecentPageBuffer() {
  if (!recentPageBufferStored_ || !recentPageBuffer_ || recentPageBufferMode_ != currentViewMode ||
      recentPageBufferScrollOffset_ != scrollOffset ||
      recentPageBufferBookCount_ != static_cast<int>(recentBooks.size())) {
    return false;
  }

  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) {
    return false;
  }

  memcpy(frameBuffer, recentPageBuffer_, renderer.getBufferSize());
  return true;
}

void RecentActivity::freeRecentPageBuffer() {
  if (recentPageBuffer_) {
    free(recentPageBuffer_);
    recentPageBuffer_ = nullptr;
  }
  recentPageBufferStored_ = false;
  recentPageBufferMode_ = ViewMode::Flow;
  recentPageBufferScrollOffset_ = -1;
  recentPageBufferBookCount_ = -1;
}

void RecentActivity::resetRecentImageCacheJobs() { recentImageCacheJobPending_ = false; }

bool RecentActivity::queueRecentImageCacheBuild(const std::string& path, int x, int y, int w, int h, bool cropToFill,
                                                BitmapRender::RoundedOutside roundedOutside) {
  if (path.empty() || w <= 0 || h <= 0) {
    return false;
  }
  if (recentImageCacheJobPending_) {
    return true;
  }

  RecentImageCacheJob& job = recentImageCacheJob_;
  job.path = path;
  job.x = x;
  job.y = y;
  job.w = w;
  job.h = h;
  job.cropToFill = cropToFill;
  job.roundedOutside = roundedOutside;
  recentImageCacheJobPending_ = true;
  return true;
}

bool RecentActivity::processNextRecentImageCacheJob() {
  if (!recentImageCacheJobPending_) {
    return false;
  }

  const RecentImageCacheJob job = recentImageCacheJob_;
  recentImageCacheJobPending_ = false;

  ImageRender::Options options;
  options.cropToFill = job.cropToFill;
  options.useDisplayCache = true;
  options.roundedOutside = job.roundedOutside;
  if (!ImageRender::create(renderer, job.path).render(job.x, job.y, job.w, job.h, options)) {
    return false;
  }
  freeRecentPageBuffer();
  updateRequired = true;
  return true;
}

void RecentActivity::pumpDisplayFromLoop() {
  if (!updateRequired) {
    return;
  }
  if (pendingInitialLoadingFrame_) {
    pendingInitialLoadingFrame_ = false;
    renderInitialLoadingFrame();
    updateRequired = true;
    return;
  }
  const bool canUseBuffer = canUseRecentPageBuffer();
  if (canUseBuffer && restoreRecentPageBuffer()) {
    drawBufferedSelectionOverlay();
    const auto target = currentViewMode == ViewMode::Icons ? GfxRenderer::ReinforcementTarget::MonochromeUi
                                                           : GfxRenderer::ReinforcementTarget::DitheredThumbnail;
    renderer.displayWithReinforcement(target);
    if (!halfRefreshOnLoadApplied_) {
      halfRefreshOnLoadApplied_ = true;
      SETTINGS.runHalfRefreshOnLoadIfEnabled(renderer, SystemSetting::RefreshOnLoadPage::Recent);
    }
    updateRequired = false;
    return;
  }

  renderer.clearScreen();
  renderTabBar(renderer);
  resetRecentImageCacheJobs();

  if (sdCardAvailable) {
    syncLayoutEngineForViewMode();
    suppressBufferedSelection_ = canUseBuffer;
    layoutEngine_->paint(*this);
    suppressBufferedSelection_ = false;
  } else {
    renderSdCardUnavailableMessage();
  }

  const auto labels = mappedInput.mapLabels("Menu", "Open", "", "");
  renderButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (canUseBuffer) {
    storeRecentPageBuffer();
    drawBufferedSelectionOverlay();
  }

  const auto target = currentViewMode == ViewMode::Icons ? GfxRenderer::ReinforcementTarget::MonochromeUi
                                                         : GfxRenderer::ReinforcementTarget::DitheredThumbnail;
  renderer.displayWithReinforcement(target);
  if (!halfRefreshOnLoadApplied_) {
    halfRefreshOnLoadApplied_ = true;
    SETTINGS.runHalfRefreshOnLoadIfEnabled(renderer, SystemSetting::RefreshOnLoadPage::Recent);
  }
  updateRequired = false;
}

/**
 * Formats milliseconds into a human-readable time string.
 * Output format: "X.X h" for hours (with one decimal), "X m" for minutes.
 */
std::string RecentActivity::formatTime(uint32_t milliseconds) const {
  char buffer[32];
  float hours = milliseconds / (1000.0f * 3600.0f);

  if (hours >= 1.0f) {
    snprintf(buffer, sizeof(buffer), "%.1f h", hours);
  } else {
    uint32_t minutes = milliseconds / (1000 * 60);
    snprintf(buffer, sizeof(buffer), "%u m", minutes);
  }
  return std::string(buffer);
}

/**
 * List (default) view: two thumbnails in the top band; stats below use the same 2×2 grid as Flow.
 */
void RecentActivity::ensureDashboardPosition(const RecentBook& book) {
  if (dashPosPath_ == book.path) {
    return;
  }
  dashPosPath_ = book.path;
  dashCurPage_ = dashChapterPages_ = dashCurChapter_ = dashTotalChapters_ = 0;
  dashBookPage_ = dashBookPages_ = 0;

  const std::string cache = book.cachePath.empty() ? epubCachePathForBookPath(book.path) : book.cachePath;

  BookProgress progress(cache);
  BookProgress::Data d;
  if (progress.load(d)) {
    dashCurPage_ = d.pageNumber + 1;
    dashChapterPages_ = d.chapterPageCount;
    dashBookPage_ = d.bookPage;
    dashBookPages_ = d.bookPageCount;
    dashCurChapter_ = d.spineIndex + 1;
  }

  BookMetadataCache meta(cache);
  if (meta.load()) {
    dashTotalChapters_ = meta.getSpineCount();
  }
}

void RecentActivity::renderStatsDashboard() {
  const int n = static_cast<int>(recentBooks.size());
  if (n == 0) {
    renderer.text.centered(ATKINSON_HYPERLEGIBLE_12_FONT_ID, renderer.getScreenHeight() / 2, "No recent books");
    return;
  }

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int coverX = 24;

  // Top is always the current (most-recent) book; the list below shows only the OTHER books.
  const RecentBook& cur = recentBooks[0];
  const CachedRecentStats& cs = statsForRecentIndex(0);
  const BookReadingStats& st = cs.stats;
  const bool hasStats = cs.loaded;
  ensureDashboardPosition(cur);

  const CachedRecentStats& cc = (n > 1) ? statsForRecentIndex(1) : cs;
  const BookReadingStats& cmp = cc.stats;
  const bool showCompare = hasStats && (n > 1) && cc.loaded;
  const ReadingDailySummary dailyStats = ReadingDailyStats::loadSummary();

  // ---- Top section: left cover/progress column, right title/author/stats column ----
  const int coverY = TAB_BAR_HEIGHT + 14;
  int coverW = screenW * 35 / 100;
  int coverH = coverW * COVER_HEIGHT / COVER_WIDTH;
  const int topSectionMaxH = screenH * 34 / 100;
  if (coverH > topSectionMaxH) {
    coverH = topSectionMaxH;
    coverW = coverH * COVER_WIDTH / COVER_HEIGHT;
  }
  const bool rr = SETTINGS.bitmapRoundedCorners != 0;
  // Draw a cover fresh (no display cache) with dark mode off, so images never render inverted. The
  // display cache captures the post-inversion framebuffer, so a cached thumbnail would stay inverted.
  auto drawCoverImage = [&](int ix, int iy, int iw, int ih, const std::string& cacheDir, const std::string& phTitle) {
    renderer.rectangle.fill(ix, iy, iw, ih, false, rr);
    const std::string img = resolveThumbnailPath(cacheDir);
    const bool dm = renderer.isDarkMode();
    if (dm) renderer.setDarkMode(false);
    bool ok = false;
    if (!img.empty()) {
      ImageRender::Options o;
      o.cropToFill = true;
      o.useDisplayCache = false;
      o.roundedOutside = rr ? BitmapRender::RoundedOutside::PaperOutside : BitmapRender::RoundedOutside::None;
      ok = ImageRender::create(renderer, img).render(ix, iy, iw, ih, o);
    }
    if (dm) renderer.setDarkMode(true);
    if (!ok) drawRecentNoCoverPlaceholder(renderer, ix, iy, iw, ih, phTitle, ATKINSON_HYPERLEGIBLE_10_FONT_ID);
  };
  const std::string curCache = cur.cachePath.empty() ? epubCachePathForBookPath(cur.path) : cur.cachePath;
  drawCoverImage(coverX, coverY, coverW, coverH, curCache, bookDisplayTitle(cur));
  if (selectorIndex == 0) {
    renderer.rectangle.render(coverX - 3, coverY - 3, coverW + 6, coverH + 6, true, rr);
    renderer.rectangle.render(coverX - 4, coverY - 4, coverW + 8, coverH + 8, true, rr);
  }

  // ---- Title spans the right side; book stats and daily stats sit in separate columns below. ----
  const int TITLE_FONT = ATKINSON_HYPERLEGIBLE_12_FONT_ID;
  const int AUTHOR_FONT = ATKINSON_HYPERLEGIBLE_10_FONT_ID;
  const int VALUE_FONT = ATKINSON_HYPERLEGIBLE_14_FONT_ID;
  const int DAILY_VALUE_FONT = ATKINSON_HYPERLEGIBLE_10_FONT_ID;
  const int LABEL_FONT = ATKINSON_HYPERLEGIBLE_8_FONT_ID;
  constexpr int kCmpIconSz = 40;
  constexpr int kCmpIconY = -3;
  constexpr int kCmpGapAfterIcon = 8;
  constexpr int kCmpValY = 8;
  const int statsX = coverX + coverW + 22;
  const int statsRight = screenW - coverX;
  const int statsW = std::max(80, statsRight - statsX);
  constexpr int kStatsColumnGap = 12;
  const int dailyColW = std::min(110, std::max(88, statsW * 32 / 100));
  const int statColW = std::max(90, statsW - kStatsColumnGap - dailyColW);
  const int dailyX = statsX + statColW + kStatsColumnGap;
  const int actualDailyColW = std::max(60, statsRight - dailyX);
  int metaY = coverY;
  const std::string hdrTitle = renderer.text.truncate(TITLE_FONT, bookDisplayTitle(cur).c_str(), statsW);
  renderer.text.render(TITLE_FONT, statsX, metaY, hdrTitle.c_str(), true, EpdFontFamily::BOLD);
  metaY += renderer.text.getLineHeight(TITLE_FONT) + 3;
  if (!cur.author.empty()) {
    const std::string hdrAuthor = renderer.text.truncate(AUTHOR_FONT, cur.author.c_str(), statsW);
    renderer.text.render(AUTHOR_FONT, statsX, metaY, hdrAuthor.c_str(), true);
    metaY += renderer.text.getLineHeight(AUTHOR_FONT) + 7;
  } else {
    metaY += 4;
  }

  const int statStartY = metaY;
  constexpr int kBookStatCount = 4;
  constexpr int kDailyStatCount = 2;
  constexpr int kStatRowGap = 6;
  const int statBlockH = renderer.text.getLineHeight(VALUE_FONT) + renderer.text.getLineHeight(LABEL_FONT) + 2;
  const int minStatSpacing = statBlockH + kStatRowGap;
  const int statAvailableH = std::max(minStatSpacing * kBookStatCount, coverY + coverH - statStartY);
  const int statSpacing = std::max(minStatSpacing, statAvailableH / kBookStatCount);
  char buf[24];
  char cmpBuf[24];

  auto drawStatBlock = [&](int x, int colW, int valueFont, int idx, int spacing, const char* valueText, bool haveCmp,
                           uint32_t curV, uint32_t othV, const char* othText, const char* label) {
    const int y = statStartY + idx * spacing;
    const std::string shownValue = renderer.text.truncate(valueFont, valueText, colW, EpdFontFamily::BOLD);
    renderer.text.render(valueFont, x, y, shownValue.c_str(), true, EpdFontFamily::BOLD);
    if (showCompare && haveCmp) {
      const int iconX = x + renderer.text.getWidth(valueFont, shownValue.c_str()) + 10;
      const int othX = iconX + kCmpIconSz + kCmpGapAfterIcon;
      if (othX < x + colW - 4) {
        if (curV > othV) {
          renderer.bitmap.icon(Up, iconX, y + kCmpIconY, kCmpIconSz, kCmpIconSz);
        } else if (curV < othV) {
          renderer.bitmap.icon(Down, iconX, y + kCmpIconY, kCmpIconSz, kCmpIconSz);
        }
        const int othW = std::max(0, x + colW - othX);
        const std::string shownOther = renderer.text.truncate(LABEL_FONT, othText, othW, EpdFontFamily::BOLD);
        renderer.text.render(LABEL_FONT, othX, y + kCmpValY, shownOther.c_str(), true, EpdFontFamily::BOLD);
      }
    }
    const std::string shownLabel = renderer.text.truncate(LABEL_FONT, label, colW);
    renderer.text.render(LABEL_FONT, x, y + renderer.text.getLineHeight(valueFont) + 1, shownLabel.c_str(), true);
  };

  std::string todayGoalStr = "-";
  std::string minPerDayStr = "-";
  if (dailyStats.hasClock) {
    const std::string todayStr = formatDailyDurationCompact(dailyStats.todayReadingMs, true);
    const std::string goalStr = formatDailyDurationCompact(dailyStats.goalReadingMs);
    todayGoalStr = todayStr + "/" + goalStr;
    minPerDayStr = formatDailyDurationCompact(dailyStats.recent7ReadingMs / 7UL);
  } else {
    todayGoalStr = "Sync RTC";
  }
  const int dailySpacing = std::max(minStatSpacing, statSpacing);
  drawStatBlock(dailyX, actualDailyColW, DAILY_VALUE_FONT, 0, dailySpacing, todayGoalStr.c_str(), false, 0, 0, "",
                "Daily Goal");
  drawStatBlock(dailyX, actualDailyColW, DAILY_VALUE_FONT, 1, dailySpacing, minPerDayStr.c_str(), false, 0, 0, "",
                "Min / Day");

  const std::string curTimeStr = formatTime(hasStats ? st.totalReadingTimeMs : 0);
  const std::string othTimeStr = formatTime(cmp.totalReadingTimeMs);
  drawStatBlock(statsX, statColW, VALUE_FONT, 0, statSpacing, curTimeStr.c_str(), true, st.totalReadingTimeMs,
                cmp.totalReadingTimeMs, othTimeStr.c_str(), "Reading Time");

  snprintf(buf, sizeof(buf), "%u", hasStats ? st.totalPagesRead : 0u);
  snprintf(cmpBuf, sizeof(cmpBuf), "%u", cmp.totalPagesRead);
  drawStatBlock(statsX, statColW, VALUE_FONT, 1, statSpacing, buf, true, st.totalPagesRead, cmp.totalPagesRead, cmpBuf,
                "Pages");

  snprintf(buf, sizeof(buf), "%u", hasStats ? st.totalChaptersRead : 0u);
  snprintf(cmpBuf, sizeof(cmpBuf), "%u", cmp.totalChaptersRead);
  drawStatBlock(statsX, statColW, VALUE_FONT, 2, statSpacing, buf, true, st.totalChaptersRead, cmp.totalChaptersRead,
                cmpBuf, "Chapters");

  snprintf(buf, sizeof(buf), "%u s", (hasStats ? st.avgPageTimeMs : 0u) / 1000);
  snprintf(cmpBuf, sizeof(cmpBuf), "%u s", cmp.avgPageTimeMs / 1000);
  drawStatBlock(statsX, statColW, VALUE_FONT, 3, statSpacing, buf, true, st.avgPageTimeMs, cmp.avgPageTimeMs, cmpBuf,
                "Average / Page");
  const int bookStatsBottom = statStartY + (kBookStatCount - 1) * statSpacing +
                              renderer.text.getLineHeight(VALUE_FONT) + renderer.text.getLineHeight(LABEL_FONT) + 4;
  const int dailyStatsBottom = statStartY + (kDailyStatCount - 1) * dailySpacing +
                               renderer.text.getLineHeight(VALUE_FONT) + renderer.text.getLineHeight(LABEL_FONT) + 4;
  const int statsBottom = std::max(bookStatsBottom, dailyStatsBottom);

  // ---- Under the cover: pages (left) / chapter (middle) / percent (right), then progress bar ----
  const int barH = 12;
  const int infoFont = ATKINSON_HYPERLEGIBLE_8_FONT_ID;
  const int infoLh = renderer.text.getLineHeight(infoFont);
  const int barY = coverY + coverH + 10 + infoLh + 4;
  const int infoY = coverY + coverH + 10;

  const float progFrac =
      (cur.progress >= 0.0f && cur.progress <= 1.0f) ? cur.progress : (hasStats ? st.progressPercent / 100.0f : -1.0f);

  char pagesTxt[24];
  if (dashBookPage_ > 0 && dashBookPages_ > 0) {
    snprintf(pagesTxt, sizeof(pagesTxt), "%d/%d", dashBookPage_, dashBookPages_);
  } else if (dashCurPage_ > 0 && dashChapterPages_ > 0) {
    snprintf(pagesTxt, sizeof(pagesTxt), "%d/%d", dashCurPage_, dashChapterPages_);
  } else {
    pagesTxt[0] = '\0';
  }
  char chapTxt[24];
  char chapPagesTxt[24];
  if (dashCurChapter_ > 0 && dashChapterPages_ > 0) {
    snprintf(chapTxt, sizeof(chapTxt), "%d: %d/%d", dashCurChapter_, dashCurPage_, dashChapterPages_);
    snprintf(chapPagesTxt, sizeof(chapPagesTxt), "%d/%d", dashCurPage_, dashChapterPages_);
  } else if (dashCurChapter_ > 0) {
    snprintf(chapTxt, sizeof(chapTxt), "%d", dashCurChapter_);
    chapPagesTxt[0] = '\0';
  } else {
    chapTxt[0] = '\0';
    chapPagesTxt[0] = '\0';
  }
  char pctTxt[8];
  if (progFrac >= 0.0f) {
    snprintf(pctTxt, sizeof(pctTxt), "%d%%", static_cast<int>(progFrac * 100.0f + 0.5f));
  } else {
    snprintf(pctTxt, sizeof(pctTxt), "-");
  }

  constexpr int kInfoGap = 6;
  const int pctW = renderer.text.getWidth(infoFont, pctTxt);
  const int pctX = coverX + coverW - pctW;
  int leftW = 0;
  if (pagesTxt[0]) {
    renderer.text.render(infoFont, coverX, infoY, pagesTxt, true);
    leftW = renderer.text.getWidth(infoFont, pagesTxt);
  }

  const int midMinX = coverX + leftW + (leftW > 0 ? kInfoGap : 0);
  const int midMaxX = pctX - kInfoGap;
  auto middleFits = [&](const char* text) {
    return text[0] && renderer.text.getWidth(infoFont, text) <= (midMaxX - midMinX);
  };
  const char* middleTxt = nullptr;
  if (middleFits(chapTxt)) {
    middleTxt = chapTxt;
  } else if (middleFits(chapPagesTxt)) {
    middleTxt = chapPagesTxt;
  }
  if (middleTxt) {
    const int midW = renderer.text.getWidth(infoFont, middleTxt);
    const int midX = midMinX + (midMaxX - midMinX - midW) / 2;
    renderer.text.render(infoFont, midX, infoY, middleTxt, true);
  }
  renderer.text.render(infoFont, pctX, infoY, pctTxt, true);

  renderer.rectangle.fill(coverX, barY, coverW, barH, false);
  renderer.rectangle.render(coverX, barY, coverW, barH, true);
  if (progFrac > 0.0f) {
    renderer.rectangle.fill(coverX, barY, static_cast<int>(coverW * progFrac + 0.5f), barH);
  }

  // ---- Other-books list (excludes the current book at index 0) ----
  const int topContentBottom = std::max(std::max(barY + barH, coverY + coverH), statsBottom);
  const int listTop0 = topContentBottom + 16;
  renderer.line.render(0, listTop0 - 8, screenW, listTop0 - 8, true);

  const int otherCount = n - 1;  // books 1..n-1
  if (otherCount <= 0) {
    return;
  }

  const int listBottom = screenH - 34;
  const int padX = 24;
  const int listAreaH = std::max(1, listBottom - listTop0);
  const int rowH = std::max(104, listAreaH / 3);
  const int thumbH = std::max(82, rowH - 22);
  const int thumbW = thumbH * COVER_WIDTH / COVER_HEIGHT;
  const int maxRows = std::max(1, (listBottom - listTop0) / rowH);

  // Window over the other-books list (list positions 0..otherCount-1 map to book indices 1..n-1),
  // scrolled to keep the selected list book visible.
  const int selListPos = (selectorIndex >= 1) ? selectorIndex - 1 : -1;
  int windowStart = 0;
  if (selListPos >= maxRows) {
    windowStart = selListPos - maxRows + 1;
  }
  const int maxWindowStart = std::max(0, otherCount - maxRows);
  if (windowStart > maxWindowStart) windowStart = maxWindowStart;

  const int fontTitle = ATKINSON_HYPERLEGIBLE_12_FONT_ID;
  const int fontSub = ATKINSON_HYPERLEGIBLE_10_FONT_ID;

  for (int row = 0; row < maxRows; ++row) {
    const int listPos = windowStart + row;
    if (listPos >= otherCount) break;
    const int bi = listPos + 1;  // book index (skip current at 0)
    const RecentBook& b = recentBooks[static_cast<size_t>(bi)];
    const int y = listTop0 + row * rowH;

    if (bi == selectorIndex) {
      renderer.rectangle.render(padX / 2, y + 2, screenW - padX, rowH - 8, true, false);
    }

    const int ty = y + (rowH - thumbH) / 2;
    const std::string cdir = b.cachePath.empty() ? epubCachePathForBookPath(b.path) : b.cachePath;
    drawCoverImage(padX, ty, thumbW, thumbH, cdir, bookDisplayTitle(b));

    const int tx = padX + thumbW + 16;
    const int tw = screenW - padX - tx;
    const int lhTitle = renderer.text.getLineHeight(fontTitle);
    const int lhSub = renderer.text.getLineHeight(fontSub);
    constexpr int rowBarH = 8;
    const int rowBarY = y + rowH - 22;
    const int titleY = y + 12;
    const std::string title = renderer.text.truncate(fontTitle, bookDisplayTitle(b).c_str(), tw);
    renderer.text.render(fontTitle, tx, titleY, title.c_str(), true, EpdFontFamily::BOLD);
    if (!b.author.empty()) {
      const std::string au = renderer.text.truncate(fontSub, b.author.c_str(), tw);
      renderer.text.render(fontSub, tx, titleY + lhTitle + 4, au.c_str(), true);
    }

    float rowProg = b.progress;
    if (rowProg < 0.0f || rowProg > 1.0f) {
      const CachedRecentStats& rowStats = statsForRecentIndex(bi);
      if (rowStats.loaded) {
        rowProg = rowStats.stats.progressPercent / 100.0f;
      }
    }
    if (rowProg >= 0.0f && rowProg <= 1.0f) {
      char prog[8];
      snprintf(prog, sizeof(prog), "%d%%", static_cast<int>(rowProg * 100.0f + 0.5f));
      const int progTextW = renderer.text.getWidth(fontSub, prog);
      const int barW = std::max(24, tw - progTextW - 10);
      renderer.rectangle.fill(tx, rowBarY, barW, rowBarH, false);
      renderer.rectangle.render(tx, rowBarY, barW, rowBarH, true);
      if (rowProg > 0.0f) {
        renderer.rectangle.fill(tx, rowBarY, static_cast<int>(barW * rowProg + 0.5f), rowBarH);
      }
      renderer.text.render(fontSub, tx + barW + 10, rowBarY - 7, prog, true);
    }
  }
}

/**
 * Main loop for handling user input and updating state.
 */
void RecentActivity::loop() {
  if (homeMenuDrawer_ && homeMenuDrawer_->visible()) {
    homeMenuDrawer_->handleInput(mappedInput);
    return;
  }

  if (updateRequired) {
    if (removeConfirmOpen_) {
      renderRemoveConfirmation();
    } else {
      pumpDisplayFromLoop();
    }
  } else if (processNextRecentImageCacheJob()) {
    return;
  }

  if (removeConfirmOpen_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelRemoveConfirmation();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      confirmRemoveRecent();
      return;
    }
    return;
  }

  const int totalBooks = static_cast<int>(recentBooks.size());
  // Stats dashboard navigates like the vertical list (Up/Down over books, Confirm opens the selected one).
  const bool isListView = (currentViewMode == ViewMode::List || currentViewMode == ViewMode::StatsDashboard);
  const bool isCoverView = (currentViewMode == ViewMode::Cover);
  const bool isIconsView = (currentViewMode == ViewMode::Icons);

  // Tab vs item nav buttons depend on the main-menu nav setting. In front mode these map to the same physical
  // keys as before (no behavior change); side mode swaps the axes (Up/Down = tabs, Left/Right = items).
  bool upPressed = mappedInput.wasPressed(itemPrevButton());
  bool downPressed = mappedInput.wasPressed(itemNextButton());
  bool leftPressed = mappedInput.wasPressed(tabPrevButton());
  bool rightPressed = mappedInput.wasPressed(tabNextButton());
  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const bool confirmNavigation = confirmReleased && mappedInput.getHeldTime() >= GO_HOME_MS;
  const bool confirmPressed = confirmReleased && !confirmNavigation;

  if (ignoreBackReleaseOnEnter_) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      return;
    }
    ignoreBackReleaseOnEnter_ = false;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (mappedInput.getHeldTime() >= GO_HOME_MS) {
      return;
    }
    openHomeMenuDrawer();
    return;
  }

  if (leftPressed) {
    tabSelectorIndex = 5;
    navigateToSelectedMenu();
    return;
  }

  if (rightPressed) {
    tabSelectorIndex = 1;
    navigateToSelectedMenu();
    return;
  }

  if (tabSelectorIndex != 0) {
    return;
  }

  if (!isCoverView && totalBooks == 0) {
    return;
  }

  {
    const ViewMode expectedMode = viewModeForLibrarySetting(SETTINGS.recentLibraryMode);
    if (expectedMode != currentViewMode) {
      freeRecentPageBuffer();
      currentViewMode = expectedMode;
      scrollOffset = 0;
      selectorIndex = 0;
      updateRequired = true;
      return;
    }
  }

  bool selectorChanged = false;

  if (isCoverView) {
    scrollOffset = 0;
    if (totalBooks == 0) {
      return;
    }
    if (selectorIndex < 0) {
      selectorIndex = 0;
    }
    if (selectorIndex >= totalBooks) {
      selectorIndex = totalBooks - 1;
    }
    if (downPressed) {
      selectorIndex = (selectorIndex + 1) % totalBooks;
      updateRequired = true;
      return;
    }
    if (upPressed) {
      selectorIndex = (selectorIndex + totalBooks - 1) % totalBooks;
      updateRequired = true;
      return;
    }
    if (confirmPressed || confirmNavigation) {
      const auto& book = recentBooks[static_cast<size_t>(selectorIndex)];
      openBookPath(book.path, book.title, book.author, true, confirmNavigation);
      return;
    }
    return;
  }

  if (isListView) {
    if (downPressed && totalBooks > 0) {
      selectorIndex = (selectorIndex + 1) % totalBooks;
      selectorChanged = true;
    }
    if (upPressed && totalBooks > 0) {
      selectorIndex = (selectorIndex + totalBooks - 1) % totalBooks;
      selectorChanged = true;
    }

    if (selectorChanged) {
      const int visibleItems = LIST_VISIBLE_ITEMS;
      if (selectorIndex < scrollOffset) {
        scrollOffset = selectorIndex;
      } else if (selectorIndex >= scrollOffset + visibleItems) {
        scrollOffset = selectorIndex - visibleItems + 1;
      }
      const int maxOffset = std::max(0, totalBooks - visibleItems);
      scrollOffset = std::max(0, std::min(scrollOffset, maxOffset));
      updateRequired = true;
    }

    if ((confirmPressed || confirmNavigation) && selectorIndex >= 0 && selectorIndex < totalBooks) {
      const auto& book = recentBooks[selectorIndex];
      openBookPath(book.path, book.title, book.author, true, confirmNavigation);
      return;
    }
  } else {
    if (downPressed && totalBooks > 0) {
      selectorIndex = (selectorIndex + 1) % totalBooks;
      selectorChanged = true;
    } else if (upPressed && totalBooks > 0) {
      selectorIndex = (selectorIndex + totalBooks - 1) % totalBooks;
      selectorChanged = true;
    }

    if (selectorChanged) {
      const int cols = isIconsView ? ICON_COLS : GRID_COLS;
      int currentRow = selectorIndex / cols;
      int visibleRows = getVisibleRows();
      if (currentRow < scrollOffset) {
        scrollOffset = currentRow;
      } else if (currentRow >= scrollOffset + visibleRows) {
        scrollOffset = currentRow - visibleRows + 1;
      }
      int totalRows = (totalBooks + cols - 1) / cols;
      int maxOffset = std::max(0, totalRows - visibleRows);
      scrollOffset = std::max(0, std::min(scrollOffset, maxOffset));
      updateRequired = true;
    }

    if ((confirmPressed || confirmNavigation) && selectorIndex >= 0 && selectorIndex < totalBooks) {
      const auto& book = recentBooks[selectorIndex];
      openBookPath(book.path, book.title, book.author, true, confirmNavigation);
      return;
    }
  }
}
