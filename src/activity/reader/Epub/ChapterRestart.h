#pragma once

/**
 * @file ChapterRestart.h
 * @brief One restart per chapter, for a chapter that will not build on a fragmented heap.
 *
 * The X3 has no PSRAM. Over a long reading session the heap fragments until the 32 KB inflate window a
 * chapter needs cannot be found in one piece, even with plenty free in total, and the chapter fails to
 * open. On a freshly booted heap the same chapter builds. So the reader records the chapter, restarts,
 * and boot reopens the book there. The record lives in RTC memory that survives a software reset, and a
 * chapter that already had its restart is not given another — so a chapter that is broken rather than
 * starved cannot loop.
 */

#include <cstdint>
#include <string>

namespace ChapterRestart {

/** True if this chapter of this book already had its restart and failed again after it. */
bool alreadyTried(const std::string& bookPath, int spineIndex);

/** Records the chapter and restarts the device. Save progress and the session before calling. */
[[noreturn]] void restartInto(const std::string& bookPath, int spineIndex);

/** Forgets the record — the chapter opened, or the reader has given up on it. */
void clear();

/**
 * True once after a chapter restart, telling boot to reopen the book whatever the boot setting says:
 * the person was mid-book, not at the home screen, when the reader restarted.
 */
bool consumeResumeRequest();

}  // namespace ChapterRestart
