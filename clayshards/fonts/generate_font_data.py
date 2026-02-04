#!/usr/bin/env python3
"""
Generate C font data from MSDF JSON + PNG files.

Usage:
    python generate_font_data.py ui-font.json ui-font.png ../src/sh_font_data.c

Output is a C file containing:
- Glyph metrics array
- Atlas pixel data (RGBA)
- Pre-built ASCII lookup table
"""

import json
import struct
import sys
import os

def load_png_rgba(png_path):
    """Load PNG and return RGBA pixels + dimensions.

    Uses a minimal PNG decoder - only handles 8-bit RGBA/RGB/grayscale.
    For production, consider using PIL/Pillow.
    """
    try:
        from PIL import Image
        img = Image.open(png_path).convert('RGBA')
        width, height = img.size
        pixels = list(img.tobytes())
        return width, height, pixels
    except ImportError:
        print("Warning: PIL not available, using built-in PNG decoder", file=sys.stderr)
        return load_png_minimal(png_path)

def load_png_minimal(png_path):
    """Minimal PNG decoder with filter support."""
    import zlib

    with open(png_path, 'rb') as f:
        # Check PNG signature
        sig = f.read(8)
        if sig != b'\x89PNG\r\n\x1a\n':
            raise ValueError("Not a valid PNG file")

        width = height = 0
        bit_depth = color_type = 0
        idat_data = b''

        while True:
            chunk_len = struct.unpack('>I', f.read(4))[0]
            chunk_type = f.read(4)
            chunk_data = f.read(chunk_len)
            f.read(4)  # CRC

            if chunk_type == b'IHDR':
                width, height, bit_depth, color_type = struct.unpack('>IIBB', chunk_data[:10])
            elif chunk_type == b'IDAT':
                idat_data += chunk_data
            elif chunk_type == b'IEND':
                break

        if bit_depth != 8:
            raise ValueError(f"Unsupported bit depth: {bit_depth}")

        # Decompress
        raw = zlib.decompress(idat_data)

        # Bytes per pixel based on color type
        bytes_per_pixel = {0: 1, 2: 3, 4: 2, 6: 4}.get(color_type, 4)
        row_bytes = width * bytes_per_pixel
        stride = 1 + row_bytes  # +1 for filter byte

        # Decode with filtering
        decoded_rows = []
        prev_row = [0] * row_bytes

        def paeth(a, b, c):
            """Paeth predictor."""
            p = a + b - c
            pa = abs(p - a)
            pb = abs(p - b)
            pc = abs(p - c)
            if pa <= pb and pa <= pc:
                return a
            elif pb <= pc:
                return b
            else:
                return c

        for y in range(height):
            row_start = y * stride
            filter_type = raw[row_start]
            row_data = list(raw[row_start + 1:row_start + stride])

            # Apply filter
            if filter_type == 0:  # None
                pass
            elif filter_type == 1:  # Sub
                for i in range(bytes_per_pixel, row_bytes):
                    row_data[i] = (row_data[i] + row_data[i - bytes_per_pixel]) & 0xFF
            elif filter_type == 2:  # Up
                for i in range(row_bytes):
                    row_data[i] = (row_data[i] + prev_row[i]) & 0xFF
            elif filter_type == 3:  # Average
                for i in range(row_bytes):
                    left = row_data[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
                    up = prev_row[i]
                    row_data[i] = (row_data[i] + (left + up) // 2) & 0xFF
            elif filter_type == 4:  # Paeth
                for i in range(row_bytes):
                    left = row_data[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
                    up = prev_row[i]
                    up_left = prev_row[i - bytes_per_pixel] if i >= bytes_per_pixel else 0
                    row_data[i] = (row_data[i] + paeth(left, up, up_left)) & 0xFF
            else:
                raise ValueError(f"Unknown PNG filter type: {filter_type}")

            decoded_rows.append(row_data)
            prev_row = row_data

        # Convert to RGBA pixels
        pixels = []
        for row in decoded_rows:
            for x in range(width):
                idx = x * bytes_per_pixel
                if color_type == 6:  # RGBA
                    pixels.extend(row[idx:idx+4])
                elif color_type == 2:  # RGB
                    pixels.extend(row[idx:idx+3])
                    pixels.append(255)  # Alpha
                elif color_type == 0:  # Grayscale
                    v = row[idx]
                    pixels.extend([v, v, v, 255])
                elif color_type == 4:  # Grayscale + Alpha
                    v = row[idx]
                    a = row[idx + 1]
                    pixels.extend([v, v, v, a])

        return width, height, pixels

def generate_c_file(json_path, png_path, output_path):
    """Generate C source file with embedded font data."""

    # Load JSON
    with open(json_path, 'r') as f:
        font_data = json.load(f)

    atlas = font_data.get('atlas', {})
    glyphs = font_data.get('glyphs', [])

    # Sort glyphs by unicode for binary search
    glyphs.sort(key=lambda g: g.get('unicode', 0))

    # Load PNG
    width, height, pixels = load_png_rgba(png_path)

    # Verify dimensions match
    if width != atlas.get('width', width) or height != atlas.get('height', height):
        print(f"Warning: PNG dimensions ({width}x{height}) don't match atlas metadata", file=sys.stderr)

    # Check yOrigin - msdf-atlas-gen uses "bottom" by default (y=0 at bottom)
    # but PNG images use top-down (y=0 at top), so we need to flip
    y_origin = atlas.get('yOrigin', 'top')
    atlas_height = atlas.get('height', height)

    # Generate C code
    lines = []
    lines.append("/*")
    lines.append(" * sh_font_data.c - Generated MSDF font data")
    lines.append(" *")
    lines.append(f" * Generated from: {os.path.basename(json_path)}, {os.path.basename(png_path)}")
    lines.append(" * DO NOT EDIT - regenerate with generate_font_data.py")
    lines.append(" */")
    lines.append("")
    lines.append('#include "sh_font.h"')
    lines.append("")

    # Generate glyph array
    lines.append(f"/* {len(glyphs)} glyphs, sorted by unicode */")
    lines.append("static const SHGlyph sh_font_ui_glyphs[] = {")

    for g in glyphs:
        unicode = g.get('unicode', 0)
        advance = g.get('advance', 0.5)
        plane = g.get('planeBounds', {})
        atlas_bounds = g.get('atlasBounds', {})

        # Get atlas bounds
        atlas_left = atlas_bounds.get('left', 0)
        atlas_right = atlas_bounds.get('right', 0)
        atlas_bottom = atlas_bounds.get('bottom', 0)
        atlas_top = atlas_bounds.get('top', 0)

        # Flip Y if yOrigin is "bottom" (convert from math coords to image coords)
        # In math coords: y=0 at bottom, y increases upward
        # In image coords: y=0 at top, y increases downward
        if y_origin == 'bottom':
            new_bottom = atlas_height - atlas_top
            new_top = atlas_height - atlas_bottom
            atlas_bottom = new_bottom
            atlas_top = new_top

        lines.append(f"    {{ {unicode}, {advance:.6f}f, "
                     f"{{ {plane.get('left', 0):.6f}f, {plane.get('bottom', 0):.6f}f, "
                     f"{plane.get('right', 0):.6f}f, {plane.get('top', 0):.6f}f }}, "
                     f"{{ {atlas_left:.1f}f, {atlas_bottom:.1f}f, "
                     f"{atlas_right:.1f}f, {atlas_top:.1f}f }} }},")

    lines.append("};")
    lines.append("")

    # Generate atlas data
    lines.append(f"/* Atlas: {width}x{height} RGBA ({len(pixels)} bytes) */")
    lines.append("static const uint8_t sh_font_ui_atlas[] = {")

    # Write pixels in rows of 16 bytes
    for i in range(0, len(pixels), 16):
        chunk = pixels[i:i+16]
        hex_vals = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"    {hex_vals},")

    lines.append("};")
    lines.append("")

    # Build ASCII mapping
    ascii_map = {}
    for i, g in enumerate(glyphs):
        unicode = g.get('unicode', 0)
        if unicode < 128:
            ascii_map[unicode] = i

    # Generate the font struct
    lines.append("/* Main font struct */")
    lines.append("const SHFont sh_font_ui = {")
    lines.append(f"    .atlas_width = {width},")
    lines.append(f"    .atlas_height = {height},")
    lines.append(f"    .distance_range = {atlas.get('distanceRange', 4.0):.1f}f,")
    lines.append(f"    .em_size = {atlas.get('size', 32.0):.1f}f,")
    lines.append(f"    .glyphs = sh_font_ui_glyphs,")
    lines.append(f"    .glyph_count = {len(glyphs)},")
    lines.append(f"    .ascii = {{")

    # Static ASCII table initialization
    for i in range(128):
        if i in ascii_map:
            lines.append(f"        &sh_font_ui_glyphs[{ascii_map[i]}],  /* {repr(chr(i)) if 32 <= i < 127 else i} */")
        else:
            lines.append(f"        NULL,  /* {i} */")

    lines.append("    },")
    lines.append(f"    .atlas_data = sh_font_ui_atlas,")
    lines.append(f"    .atlas_data_size = sizeof(sh_font_ui_atlas),")
    lines.append("};")
    lines.append("")

    # Write output
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines))

    print(f"Generated {output_path}:")
    print(f"  - {len(glyphs)} glyphs")
    print(f"  - {width}x{height} atlas ({len(pixels)} bytes)")
    print(f"  - {len(ascii_map)} ASCII mappings")

if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <font.json> <font.png> <output.c>", file=sys.stderr)
        sys.exit(1)

    generate_c_file(sys.argv[1], sys.argv[2], sys.argv[3])
