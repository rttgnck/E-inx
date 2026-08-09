#include "StudyFonts.h"

namespace study {

namespace {
// Kept even though nothing loads them: the names are what the card template
// randomises over, and a future real loader wants the same list in the same
// order. familyName() is also read for display, so an empty table would show a
// blank where upstream shows a face name.
constexpr const char* kFamilies[StudyFonts::kFamilyCount] = {"SimSun", "SimHei", "MicrosoftYaHei", "KaiTi", "FangSong"};
}  // namespace

const char* StudyFonts::familyName(const int index) {
  if (index < 0 || index >= kFamilyCount) return "";
  return kFamilies[index];
}

bool StudyFonts::load(GfxRenderer&, int) { return false; }

int StudyFonts::loadPreferred(GfxRenderer&, int) { return -1; }

void StudyFonts::unload(GfxRenderer&) {}

void StudyFonts::prewarm(GfxRenderer&, const char*, const char*) const {}

}  // namespace study
