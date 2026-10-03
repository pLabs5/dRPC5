#!/usr/bin/env python3
import argparse
import os
import struct
import zlib

FONT = {
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "C": ["01111", "10000", "10000", "10000", "10000", "10000", "01111"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
}


def make_pixels(size):
    bg = (18, 19, 26, 255)
    fg = (88, 101, 242, 255)
    tx = (255, 255, 255, 255)
    px = [bytearray(bg * size) for _ in range(size)]

    for y in range(size):
        for x in range(size):
            if 40 <= x < size - 40 and 40 <= y < size - 40:
                px[y][x * 4 : x * 4 + 4] = bytes(fg)

    text = "DRPC5"
    scale = 52
    char_w, char_h = 5, 7
    gap = 1
    total_w = (len(text) * (char_w + gap) - gap) * scale
    total_h = char_h * scale
    ox = (size - total_w) // 2
    oy = (size - total_h) // 2

    cx = ox
    for ch in text:
        glyph = FONT[ch]
        for gy in range(char_h):
            row = glyph[gy]
            for gx in range(char_w):
                if row[gx] == "1":
                    for sy in range(scale):
                        y = oy + gy * scale + sy
                        base = cx + gx * scale
                        for sx in range(scale):
                            x = base + sx
                            px[y][x * 4 : x * 4 + 4] = bytes(tx)
        cx += (char_w + gap) * scale

    return px


def write_png(path, px):
    size = len(px)
    raw = b"".join(b"\x00" + bytes(row) for row in px)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", type=int, default=512)
    args = ap.parse_args()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    write_png(args.out, make_pixels(args.size))


if __name__ == "__main__":
    main()
