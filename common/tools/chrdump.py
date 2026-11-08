"""Dump CHR ROM tiles to a grayscale PNG sheet (16 tiles per row, 1 KiB per block row group)."""
import argparse

from PIL import Image

import nesrom

GRAYS = (0, 85, 170, 255)


def decode_tile(data: bytes) -> list[list[int]]:
    rows = []
    for y in range(8):
        lo, hi = data[y], data[y + 8]
        rows.append([((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1) for x in range(8)])
    return rows


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("rom")
    ap.add_argument("out")
    ap.add_argument("--scale", type=int, default=2)
    args = ap.parse_args()

    rom = nesrom.load(args.rom)
    tiles = len(rom.chr) // 16
    cols = 16
    rows = (tiles + cols - 1) // cols
    img = Image.new("L", (cols * 8, rows * 8))
    px = img.load()
    for t in range(tiles):
        tile = decode_tile(rom.chr[t * 16:(t + 1) * 16])
        ox, oy = (t % cols) * 8, (t // cols) * 8
        for y, row in enumerate(tile):
            for x, v in enumerate(row):
                px[ox + x, oy + y] = GRAYS[v]
    if args.scale > 1:
        img = img.resize((img.width * args.scale, img.height * args.scale), Image.NEAREST)
    img.save(args.out)
    print(f"{tiles} tiles -> {args.out}")


if __name__ == "__main__":
    main()
