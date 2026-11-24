#pragma once

#include "state/SystemSetting.h"

/**
 * @file Fonts.h
 * @brief Built-in bitmap font headers and numeric font IDs.
 */

#include "font/atkinson_hyperlegible_10_bold.h"
#include "font/atkinson_hyperlegible_10_bolditalic.h"
#include "font/atkinson_hyperlegible_10_italic.h"
#include "font/atkinson_hyperlegible_10_regular.h"
#include "font/atkinson_hyperlegible_12_bold.h"
#include "font/atkinson_hyperlegible_12_bolditalic.h"
#include "font/atkinson_hyperlegible_12_italic.h"
#include "font/atkinson_hyperlegible_12_regular.h"
#include "font/atkinson_hyperlegible_14_bold.h"
#include "font/atkinson_hyperlegible_14_bolditalic.h"
#include "font/atkinson_hyperlegible_14_italic.h"
#include "font/atkinson_hyperlegible_14_regular.h"
#include "font/atkinson_hyperlegible_16_bold.h"
#include "font/atkinson_hyperlegible_16_bolditalic.h"
#include "font/atkinson_hyperlegible_16_italic.h"
#include "font/atkinson_hyperlegible_16_regular.h"
#include "font/atkinson_hyperlegible_18_bold.h"
#include "font/atkinson_hyperlegible_18_bolditalic.h"
#include "font/atkinson_hyperlegible_18_italic.h"
#include "font/atkinson_hyperlegible_18_regular.h"
#include "font/atkinson_hyperlegible_8_regular.h"
#include "font/literata_10_bold.h"
#include "font/literata_10_bolditalic.h"
#include "font/literata_10_italic.h"
#include "font/literata_10_regular.h"
#include "font/literata_12_bold.h"
#include "font/literata_12_bolditalic.h"
#include "font/literata_12_italic.h"
#include "font/literata_12_regular.h"
#include "font/literata_14_bold.h"
#include "font/literata_14_bolditalic.h"
#include "font/literata_14_italic.h"
#include "font/literata_14_regular.h"
#include "font/literata_16_bold.h"
#include "font/literata_16_bolditalic.h"
#include "font/literata_16_italic.h"
#include "font/literata_16_regular.h"
#include "font/literata_18_bold.h"
#include "font/literata_18_bolditalic.h"
#include "font/literata_18_italic.h"
#include "font/literata_18_regular.h"
#include "font/montserrat_10_bold.h"
#include "font/montserrat_10_bolditalic.h"
#include "font/montserrat_10_italic.h"
#include "font/montserrat_10_regular.h"
#include "font/montserrat_12_bold.h"
#include "font/montserrat_12_bolditalic.h"
#include "font/montserrat_12_italic.h"
#include "font/montserrat_12_regular.h"
#include "font/montserrat_14_bold.h"
#include "font/montserrat_14_bolditalic.h"
#include "font/montserrat_14_italic.h"
#include "font/montserrat_14_regular.h"
#include "font/montserrat_16_bold.h"
#include "font/montserrat_16_bolditalic.h"
#include "font/montserrat_16_italic.h"
#include "font/montserrat_16_regular.h"
#include "font/montserrat_18_bold.h"
#include "font/montserrat_18_bolditalic.h"
#include "font/montserrat_18_italic.h"
#include "font/montserrat_18_regular.h"
#include "font/montserrat_8_regular.h"
#include "font/montserrat_clock_70_bold.h"
#include "font/montserrat_clock_70_regular.h"

#define ATKINSON_HYPERLEGIBLE_8_FONT_ID (2001)
#define ATKINSON_HYPERLEGIBLE_10_FONT_ID (2002)
#define ATKINSON_HYPERLEGIBLE_12_FONT_ID (2003)
#define ATKINSON_HYPERLEGIBLE_14_FONT_ID (2004)
#define ATKINSON_HYPERLEGIBLE_16_FONT_ID (2005)
#define ATKINSON_HYPERLEGIBLE_18_FONT_ID (2006)

// The interface font. Old (E-inx) screens name the ATKINSON_* ids and the new upstream Inx screens the
// MONTSERRAT_* ids; FontManager::applyUiFont() points both sets at the one font the "New UI" setting
// picks, so every screen uses the same face.
#define MONTSERRAT_8_FONT_ID (2501)
#define MONTSERRAT_10_FONT_ID (2502)
#define MONTSERRAT_12_FONT_ID (2503)
#define MONTSERRAT_14_FONT_ID (2504)
#define MONTSERRAT_16_FONT_ID (2505)
#define MONTSERRAT_18_FONT_ID (2506)

#define LITERATA_10_FONT_ID (3001)
#define LITERATA_12_FONT_ID (3002)
#define LITERATA_14_FONT_ID (3003)
#define LITERATA_16_FONT_ID (3004)
#define LITERATA_18_FONT_ID (3005)

#define MONTSERRAT_CLOCK_70_FONT_ID (4001)

/**
 * @brief Returns the configured bitmap font used by the device UI.
 *
 * Reader text uses its own ReaderSetting font size and does not use this helper.
 */
inline int systemFontId() {
  switch (SETTINGS.systemTextSize) {
    case SystemSetting::SYSTEM_TEXT_MEDIUM:
      return MONTSERRAT_12_FONT_ID;
    case SystemSetting::SYSTEM_TEXT_LARGE:
      return MONTSERRAT_14_FONT_ID;
    case SystemSetting::SYSTEM_TEXT_SMALL:
    default:
      return MONTSERRAT_10_FONT_ID;
  }
}
