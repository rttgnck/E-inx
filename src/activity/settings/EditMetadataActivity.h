#pragma once

/**
 * @file EditMetadataActivity.h
 * @brief Edit a book's title/author (and favorite flag) without touching its statistics.
 *
 * Statistics live in a cache dir keyed by a hash of the book's file path, so editing metadata never
 * affects them. This writes a title/author override into the book's cache dir (applied by Epub on load)
 * and updates the denormalized copies in the library (BookState) and recent (RecentBooks) lists.
 */

#include <functional>
#include <string>

#include "activity/ActivityWithSubactivity.h"

class EditMetadataActivity final : public ActivityWithSubactivity {
 public:
  EditMetadataActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                       std::string initialTitle, std::string initialAuthor, std::string initialLanguage,
                       bool initialFavorite,
                       const std::function<void()>& onBack)
      : ActivityWithSubactivity("EditMetadata", renderer, mappedInput),
        bookPath(std::move(bookPath)),
        title(std::move(initialTitle)),
        author(std::move(initialAuthor)),
        language(std::move(initialLanguage)),
        favorite(initialFavorite),
        onBack(onBack) {}

  void onEnter() override;
  void loop() override;

 private:
  std::string bookPath;
  std::string title;
  std::string author;
  std::string language;
  bool favorite = false;
  int selected = 0;  ///< 0=Title, 1=Author, 2=Language, 3=Favorite, 4=Clear Cache, 5=Save
  std::string statusMessage;
  std::function<void()> onBack;

  void render();
  void activateSelected();
  void clearBookCache();
  void save();
};
