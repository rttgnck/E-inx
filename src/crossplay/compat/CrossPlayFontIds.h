#pragma once

/**
 * @file CrossPlayFontIds.h
 * @brief CrossPoint's built-in font ids, mapped onto E-inx's.
 *
 * Upstream's `src/fontIds.h` is generated: each id is an FNV-ish hash of a
 * generated font's name, and the fonts themselves are the reader's built-ins.
 * E-inx has its own built-ins under different names and plain numeric ids, so
 * this is the translation table.
 *
 * Only the ids a ported app actually names are here. Adding a row when
 * something needs one is a smaller change than transcribing all of upstream's.
 *
 * Noto Serif is upstream's reading face; Literata is what E-inx sets books in,
 * so it is the honest counterpart at each size. See ui/ToyboxFonts.h for the
 * same reasoning applied to the Toybox cuts.
 */

#include "system/Fonts.h"

#define NOTOSERIF_12_FONT_ID LITERATA_12_FONT_ID
#define NOTOSERIF_14_FONT_ID LITERATA_14_FONT_ID
#define NOTOSERIF_16_FONT_ID LITERATA_16_FONT_ID
#define NOTOSERIF_18_FONT_ID LITERATA_18_FONT_ID
