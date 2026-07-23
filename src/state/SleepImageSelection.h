#pragma once

/**
 * @file SleepImageSelection.h
 * @brief Per-wallpaper include/exclude state for random sleep image shuffle.
 */

#include <string>

bool isSleepImageShuffleEnabled(const std::string& path);
void setSleepImageShuffleEnabled(const std::string& path, bool enabled);

