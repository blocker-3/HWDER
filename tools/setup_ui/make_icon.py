#!/usr/bin/env python3
"""Writes tools/setup_ui/hwder.ico, the HWDER Setup.exe icon: python tools/setup_ui/make_icon.py

Plain uncompressed 32-bit BMP frames (no PNG), so the icon adds no compressed, high-entropy
data to the exe. The output is deterministic; rerun after changing the drawing.
"""
from pathlib import Path
import struct

SIZES = (16, 24, 32, 48, 64, 128)
BACK_TOP, BACK_BOTTOM = (0x23, 0x6b, 0x4e), (0x12, 0x3d, 0x2c)
LETTER, BAR = (0xff, 0xff, 0xff), (0xe8, 0xb9, 0x3c)


def coverage(u, v):
    """0 = transparent, 1 = background, 2 = letter, 3 = bar, for a point in the unit square."""
    r, x, y = 0.18, min(u, 1 - u), min(v, 1 - v)
    if x < r and y < r and (r - x) ** 2 + (r - y) ** 2 > r * r:
        return 0
    if 0.40 <= v <= 0.52 and 0.26 <= u <= 0.74:
        return 3
    if 0.20 <= v <= 0.80 and (0.24 <= u <= 0.38 or 0.62 <= u <= 0.76):
        return 2
    return 1


def frame(size):
    """Bottom-up BGRA rows (4x4 supersampled) plus the 1-bit AND mask."""
    pixels = bytearray()
    for row in range(size - 1, -1, -1):
        for col in range(size):
            totals, alpha = [0.0, 0.0, 0.0], 0
            for sy in range(4):
                for sx in range(4):
                    u, v = (col + (sx + 0.5) / 4) / size, (row + (sy + 0.5) / 4) / size
                    kind = coverage(u, v)
                    if not kind:
                        continue
                    colour = LETTER if kind == 2 else BAR if kind == 3 else \
                        tuple(round(a + (b - a) * v) for a, b in zip(BACK_TOP, BACK_BOTTOM))
                    totals = [t + c for t, c in zip(totals, colour)]
                    alpha += 1
            rgb = [round(t / alpha) if alpha else 0 for t in totals]
            pixels += bytes((rgb[2], rgb[1], rgb[0], round(alpha * 255 / 16)))
    mask_row = ((size + 31) // 32) * 4
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(pixels) + mask_row * size, 0, 0, 0, 0)
    return header + bytes(pixels) + bytes(mask_row * size)


def main():
    frames = [frame(s) for s in SIZES]
    out = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    for size, data in zip(SIZES, frames):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    target = Path(__file__).with_name("hwder.ico")
    target.write_bytes(out + b"".join(frames))
    print(target, len(out) + sum(map(len, frames)), "bytes")


if __name__ == "__main__":
    main()
