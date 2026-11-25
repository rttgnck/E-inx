#!/usr/bin/env python3
"""Converts a bundled font header (src/system/font/<name>.h, from fontconvert.py) into an SD streaming
font (.bin) that FontManager/ExternalFont load from /fonts/<Family>/<Style>_<size>.bin.

The glyph metrics and bitmaps are copied byte for byte, so a font moved out of flash renders exactly as it
did built in. Format (little-endian), as written by data/js/inx_font_pack.js:
  u32 magic 'EPDF' (0x45504446), u32 version 1, u16 nameLen, name,
  i16 lineHeight, i16 ascender, i16 descender, u8 is2Bit, u16 intervalCount (0),
  u32 glyphCount, glyphCount x 24-byte rows sorted by code point
  (u16 w, u16 h, u16 advanceX, i16 left, i16 top, u32 dataLength, u32 dataOffset, u32 codepoint, u16 0),
  then the bitmap bytes.

Usage: export_sd_font.py <header.h> <style name> <out.bin>
"""
import re
import struct
import sys


def parse_header(path):
    text = open(path, encoding="utf-8").read()
    bitmap_body = re.search(r"Bitmaps\[\d*\]\s*=\s*\{(.*?)\};", text, re.S).group(1)
    bitmap = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{1,2})\b", bitmap_body))

    glyph_body = re.search(r"EpdGlyph \w+Glyphs\[\]\s*=\s*\{(.*?)\n\};", text, re.S).group(1)
    glyphs = [tuple(int(x) for x in m.split(","))
              for m in re.findall(r"\{\s*(-?\d+\s*,\s*-?\d+\s*,\s*-?\d+\s*,\s*-?\d+\s*,\s*-?\d+\s*,\s*-?\d+\s*,\s*-?\d+)\s*\}",
                                  glyph_body)]

    interval_body = re.search(r"EpdUnicodeInterval \w+Intervals\[\]\s*=\s*\{(.*?)\n\};", text, re.S).group(1)
    intervals = [tuple(int(x, 0) for x in m) for m in
                 re.findall(r"\{\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\}",
                            interval_body)]

    data = re.search(r"EpdFontData \w+\s*=\s*\{([^}]*)\}", text, re.S).group(1)
    fields = [f.strip() for f in data.split(",") if f.strip()]
    interval_count, advance_y, ascender, descender = (int(f, 0) for f in fields[3:7])
    is_2bit = fields[7] == "true"
    if interval_count != len(intervals):
        sys.exit(f"{path}: {interval_count} intervals declared, {len(intervals)} parsed")
    return bitmap, glyphs, intervals, advance_y, ascender, descender, is_2bit


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    header, style, out_path = sys.argv[1:]
    bitmap, glyphs, intervals, advance_y, ascender, descender, is_2bit = parse_header(header)

    rows = []
    for first, last, offset in intervals:
        for i, cp in enumerate(range(first, last + 1)):
            w, h, adv, left, top, length, data_offset = glyphs[offset + i]
            rows.append((cp, w, h, adv, left, top, length, data_offset))
    rows.sort()
    if len(rows) != len(glyphs):
        sys.exit(f"{header}: intervals cover {len(rows)} glyphs, table has {len(glyphs)}")

    name = style.encode("utf-8")
    out = bytearray()
    out += struct.pack("<IIH", 0x45504446, 1, len(name)) + name
    out += struct.pack("<hhhBHI", advance_y, ascender, descender, 1 if is_2bit else 0, 0, len(rows))
    for cp, w, h, adv, left, top, length, data_offset in rows:
        out += struct.pack("<HHHhhIIIH", w, h, adv, left, top, length, data_offset, cp, 0)
    out += bitmap
    with open(out_path, "wb") as f:
        f.write(out)
    print(f"{out_path}: {len(rows)} glyphs, {len(out)} bytes")


if __name__ == "__main__":
    main()
