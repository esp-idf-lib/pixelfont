#!/usr/bin/env python3
"""
pixelfont.py - turn a standard bitmap font into a C source for pixelfont.

Reads BDF (X11), PSF1 and PSF2 (Linux console) fonts, optionally compressed
with gzip, and writes a C file with a `const pixelfont_t` that can be used
directly, without loading anything at run time.

The characters can be cut down to the ones the application needs: a full
Terminus face is about 37 KB of flash, a Latin-Cyrillic subset is a few
kilobytes.

Examples:

    # the whole font
    tools/pixelfont.py font.bdf -o fonts/terminus_8x16.c --name terminus_8x16

    # only the characters of the interface
    tools/pixelfont.py term-u16n.bdf -o fonts/ui.c --name ui_font \
        --ranges 0x20-0x7e,0x400-0x4ff

    # only the characters that are used in a text file
    tools/pixelfont.py font.psfu -o fonts/menu.c --name menu_font --text ui.txt

License: MIT
"""

import argparse
import gzip
import os
import re
import struct
import sys

PSF1_MAGIC = b"\x36\x04"
PSF2_MAGIC = b"\x72\xb5\x4a\x86"


class Font:
    def __init__(self, width, height, ascent, source):
        self.width = width
        self.height = height
        self.ascent = ascent
        self.source = source
        # code point -> (advance, bitmap bytes in the cell, row-major)
        self.glyphs = {}

    def add(self, codepoint, advance, width, height, x, y, rows, stride):
        if codepoint < 0 or codepoint > 0x10FFFF:
            return
        if codepoint in self.glyphs:
            return

        cell_stride = (self.width + 7) // 8
        cell = bytearray(cell_stride * self.height)
        for sy in range(height):
            dy = y + sy
            if dy < 0 or dy >= self.height:
                continue
            for sx in range(width):
                dx = x + sx
                if dx < 0 or dx >= self.width:
                    continue
                if not rows[sy * stride + (sx >> 3)] & (0x80 >> (sx & 7)):
                    continue
                cell[dy * cell_stride + (dx >> 3)] |= 0x80 >> (dx & 7)

        self.glyphs[codepoint] = (advance or self.width, bytes(cell))

    def subset(self, ranges=None, text=None):
        keep = set()
        if ranges:
            for first, last in ranges:
                keep.update(range(first, last + 1))
        if text is not None:
            keep.update(ord(ch) for ch in text)

        if keep:
            keep.add(0x3F)      # '?' is the substitution glyph
            keep.add(0xFFFD)    # and the explicit replacement character
            self.glyphs = {cp: g for cp, g in self.glyphs.items() if cp in keep}


def parse_ranges(spec):
    ranges = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        m = re.fullmatch(r"(0x[0-9a-fA-F]+|\d+)(?:-(0x[0-9a-fA-F]+|\d+))?", part)
        if not m:
            raise SystemExit(f"pixelfont.py: bad range: {part}")
        first = int(m.group(1), 0)
        last = int(m.group(2), 0) if m.group(2) else first
        if last < first:
            raise SystemExit(f"pixelfont.py: bad range: {part}")
        ranges.append((first, last))
    return ranges


def parse_bdf(data):
    fbb = re.search(rb"^FONTBOUNDINGBOX\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)",
                    data, re.MULTILINE)
    if not fbb:
        raise SystemExit("pixelfont.py: no FONTBOUNDINGBOX in the BDF")

    width, height, fbb_x, fbb_y = (int(v) for v in fbb.groups())

    ascent = None
    m = re.search(rb"^FONT_ASCENT\s+(\d+)", data, re.MULTILINE)
    if m:
        ascent = int(m.group(1))
    if ascent is None:
        ascent = height + fbb_y

    font = Font(width, height, ascent, "bdf")
    text = data.decode("latin-1")

    # The glyph coordinates inside a BDF are the coordinates inside the font
    # bounding box, so ENCODING/BBX can be read as is: add() places the glyph
    # at (bbx_x, (fbb_y + fbb_h) - (bbx_y + bbx_h)) relative to the cell.
    lines = text.splitlines()
    i = 0
    count = len(lines)
    while i < count:
        line = lines[i].strip()
        if line.startswith("ENCODING"):
            parts = line.split()
            codepoint = int(parts[1])
            advance = width
            bbx_w = bbx_h = bbx_x = bbx_y = 0
            rows = b""
            stride = 0

            i += 1
            while i < count:
                line = lines[i].strip()
                if line.startswith("DWIDTH"):
                    advance = int(line.split()[1])
                elif line.startswith("BBX"):
                    parts = line.split()
                    bbx_w, bbx_h, bbx_x, bbx_y = (int(v) for v in parts[1:5])
                elif line.startswith("BITMAP"):
                    stride = (bbx_w + 7) // 8
                    raw = bytearray()
                    for _ in range(bbx_h):
                        i += 1
                        raw.extend(bytes.fromhex(lines[i].strip()))
                    rows = bytes(raw)
                    break
                elif line.startswith("ENDCHAR"):
                    break
                i += 1

            if rows:
                y = (fbb_y + height) - (bbx_y + bbx_h)
                font.add(codepoint, advance, bbx_w, bbx_h, bbx_x - fbb_x, y, rows, stride)
        i += 1

    return font


def parse_psf1(data):
    if len(data) < 4:
        raise SystemExit("pixelfont.py: truncated PSF1")

    mode = data[2]
    height = data[3]
    glyph_count = 512 if mode & 1 else 256
    has_table = bool(mode & 2)

    if len(data) < 4 + glyph_count * height:
        raise SystemExit("pixelfont.py: truncated PSF1 glyphs")

    font = Font(8, height, height, "psf1")
    glyphs = data[4:4 + glyph_count * height]
    table = data[4 + glyph_count * height:]
    p = 0

    for index in range(glyph_count):
        glyph = glyphs[index * height:(index + 1) * height]
        added = False

        if has_table:
            while p + 1 < len(table):
                cp = table[p] | (table[p + 1] << 8)
                p += 2
                if cp == 0xFFFF:
                    break
                font.add(cp, 8, 8, height, 0, 0, glyph, 1)
                added = True

        if not added:
            font.add(index, 8, 8, height, 0, 0, glyph, 1)

    return font


def parse_psf2(data):
    if len(data) < 32:
        raise SystemExit("pixelfont.py: truncated PSF2")

    header_size, flags, glyph_count, char_size, height, width = \
        struct.unpack_from("<6I", data, 8)

    if not glyph_count or not char_size or not width or not height:
        raise SystemExit("pixelfont.py: bad PSF2 header")
    if width > 255 or height > 255:
        raise SystemExit("pixelfont.py: the cell is too big")

    stride = (width + 7) // 8
    if stride * height > char_size:
        raise SystemExit("pixelfont.py: bad PSF2 char size")
    if len(data) < header_size + glyph_count * char_size:
        raise SystemExit("pixelfont.py: truncated PSF2 glyphs")

    font = Font(width, height, height, "psf2")
    base = header_size
    table = data[base + glyph_count * char_size:]

    if not flags & 1 or not table:
        for index in range(glyph_count):
            glyph = data[base + index * char_size:base + (index + 1) * char_size]
            font.add(index, width, width, height, 0, 0, glyph, stride)
        return font

    p = 0
    for index in range(glyph_count):
        glyph = data[base + index * char_size:base + (index + 1) * char_size]
        added = False

        while p < len(table) and table[p] != 0xFF:
            cp, size = decode_utf8(table, p)
            p += size
            font.add(cp, width, width, height, 0, 0, glyph, stride)
            added = True

        if p < len(table):
            p += 1

        if not added:
            font.add(index, width, width, height, 0, 0, glyph, stride)

    return font


def decode_utf8(data, offset):
    b0 = data[offset]
    if b0 < 0x80:
        return b0, 1
    if b0 & 0xE0 == 0xC0:
        return ((b0 & 0x1F) << 6) | (data[offset + 1] & 0x3F), 2
    if b0 & 0xF0 == 0xE0:
        return (((b0 & 0x0F) << 12) | ((data[offset + 1] & 0x3F) << 6)
                | (data[offset + 2] & 0x3F), 3)
    return (((b0 & 0x07) << 18) | ((data[offset + 1] & 0x3F) << 12)
            | ((data[offset + 2] & 0x3F) << 6) | (data[offset + 3] & 0x3F), 4)


def load(path):
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)

    if data.startswith(b"STARTFONT"):
        return parse_bdf(data)
    if data[:2] == PSF1_MAGIC:
        return parse_psf1(data)
    if data[:4] == PSF2_MAGIC:
        return parse_psf2(data)

    raise SystemExit(f"pixelfont.py: {path} is not a BDF, PSF1 or PSF2 font")


def emit(font, name, output, comment):
    glyphs = sorted(font.glyphs.items())
    if not glyphs:
        raise SystemExit("pixelfont.py: the subset is empty")

    stride = (font.width + 7) // 8
    cell_size = stride * font.height

    lines = []
    lines.append("/*")
    for line in comment.splitlines():
        lines.append(f" * {line}".rstrip())
    lines.append(" *")
    lines.append(" * Generated by tools/pixelfont.py, do not edit.")
    lines.append(" */")
    lines.append("")
    lines.append('#include "pixelfont.h"')
    lines.append("")

    # The tables are generated byte by byte, so keep the formatter away from them
    lines.append("// *INDENT-OFF*")
    lines.append("")
    lines.append(f"static const pixelfont_glyph_t {name}_glyphs[] = {{")
    for i, (cp, (advance, _)) in enumerate(glyphs):
        lines.append(f"    {{ 0x{cp:04x}, {i * cell_size}, {advance}, {{ 0, 0, 0 }} }},")
    lines.append("};")
    lines.append("")

    lines.append(f"static const uint8_t {name}_bitmaps[] = {{")
    for cp, (_, bitmap) in glyphs:
        lines.append(f"    /* U+{cp:04X} */")
        for row in range(font.height):
            chunk = bitmap[row * stride:(row + 1) * stride]
            lines.append("    " + " ".join(f"0x{b:02x}," for b in chunk))
    lines.append("};")
    lines.append("")

    lines.append(f"const pixelfont_t {name} = {{")
    lines.append(f"    .width = {font.width},")
    lines.append(f"    .height = {font.height},")
    lines.append(f"    .ascent = {font.ascent},")
    lines.append(f"    .count = {len(glyphs)},")
    lines.append(f"    .glyphs = {name}_glyphs,")
    lines.append(f"    .bitmaps = {name}_bitmaps,")
    lines.append("};")
    lines.append("")
    lines.append("// *INDENT-ON*")
    lines.append("")

    with open(output, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))

    return len(glyphs), len(glyphs) * cell_size + len(glyphs) * 12


def emit_bdf(font, name, output, comment):
    """Writes the glyphs as a BDF, so a subset can be loaded at run time."""
    glyphs = sorted(font.glyphs.items())
    if not glyphs:
        raise SystemExit("pixelfont.py: the subset is empty")

    stride = (font.width + 7) // 8

    lines = ["STARTFONT 2.1"]
    lines.append(f"FONT -pixelfont-{name}-Medium-R-Normal--{font.height}-{font.height * 10}-72-72-C-"
                 f"{font.width * 10}-ISO10646-1")
    lines.append(f"SIZE {font.height} 72 72")
    lines.append(f"FONTBOUNDINGBOX {font.width} {font.height} 0 0")
    lines.append("STARTPROPERTIES 4")
    lines.append(f"FONT_ASCENT {font.ascent}")
    lines.append(f"FONT_DESCENT {font.height - font.ascent}")
    lines.append("DEFAULT_CHAR 63")
    lines.append(f"COMMENT {comment}".rstrip())
    lines.append("ENDPROPERTIES")
    lines.append(f"CHARS {len(glyphs)}")

    for cp, (advance, bitmap) in glyphs:
        lines.append(f"STARTCHAR U+{cp:04X}")
        lines.append(f"ENCODING {cp}")
        lines.append(f"SWIDTH {advance * 1000 // max(font.width, 1)} 0")
        lines.append(f"DWIDTH {advance} 0")
        lines.append(f"BBX {font.width} {font.height} 0 0")
        lines.append("BITMAP")
        for row in range(font.height):
            chunk = bitmap[row * stride:(row + 1) * stride]
            lines.append("".join(f"{b:02X}" for b in chunk))
        lines.append("ENDCHAR")

    lines.append("ENDFONT")
    lines.append("")

    with open(output, "w", encoding="ascii") as f:
        f.write("\n".join(lines))

    return len(glyphs)


def main():
    parser = argparse.ArgumentParser(
        description="Convert a bitmap font to a C source for pixelfont.")
    parser.add_argument("input", help="BDF, PSF1 or PSF2 file, gzip is allowed")
    parser.add_argument("-o", "--output", help="C file to write")
    parser.add_argument("--name", help="name of the font object in C")
    parser.add_argument("--ranges", help="code points to keep, e.g. 0x20-0x7e,0x400-0x4ff")
    parser.add_argument("--text", help="file with the text whose characters to keep")
    parser.add_argument("--comment", help="text for the header of the generated file")
    parser.add_argument("--bdf-out",
                        help="write the subset as a BDF for pixelfont_load(), instead of C")
    parser.add_argument("--list", action="store_true", help="print the coverage and exit")
    args = parser.parse_args()

    font = load(args.input)

    text = None
    if args.text:
        with open(args.text, encoding="utf-8") as f:
            text = f.read()

    if args.ranges or text is not None:
        font.subset(parse_ranges(args.ranges) if args.ranges else None, text)

    if args.list:
        cps = sorted(font.glyphs)
        print(f"{os.path.basename(args.input)}: {font.width}x{font.height}, "
              f"{len(cps)} glyphs, U+{cps[0]:04X}..U+{cps[-1]:04X}")
        return 0

    if not args.bdf_out and (not args.output or not args.name):
        parser.error("--output and --name are required unless --list or --bdf-out is used")

    if args.name is None:
        args.name = os.path.splitext(os.path.basename(args.input))[0]

    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.name):
        parser.error(f"bad C name: {args.name}")

    comment = args.comment or f"Font from {os.path.basename(args.input)}"
    if args.bdf_out:
        count = emit_bdf(font, args.name, args.bdf_out, comment)
        print(f"{args.bdf_out}: {font.width}x{font.height}, {count} glyphs")
        return 0

    count, size = emit(font, args.name, args.output, comment)
    print(f"{args.output}: {font.width}x{font.height}, {count} glyphs, "
          f"{size} bytes, {size / 1024:.1f} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
