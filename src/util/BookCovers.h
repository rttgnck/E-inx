#pragma once

/**
 * @file BookCovers.h
 * @brief Rebuilds a book's thumbnail and cover images from the book file.
 */

#include <string>

class GfxRenderer;

namespace BookCovers {

/** True for the formats that have generated covers (EPUB, XTC/XTCH). */
bool supports(const std::string& bookPath);

/**
 * Deletes the book's thumbnails and cover images and makes a new thumbnail from the book's own cover. The cover
 * images used by the cover page and sleep screen are made again the next time they are needed; their saved
 * full-screen drawings are dropped too, since the re-extracted cover is byte for byte the same file and would
 * otherwise hit the old drawing in the display cache. Runs on its own 12 KB task (decoding a large cover needs
 * more stack than the loop task has) and blocks until it finishes. Returns true when a new thumbnail was written.
 */
bool regenerate(const std::string& bookPath, GfxRenderer& renderer);

}  // namespace BookCovers
