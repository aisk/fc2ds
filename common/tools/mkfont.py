"""Build a small bitmap font for the NDS runtime from a BDF font.

Only the characters found in the given text files (plus printable ASCII) are
kept. Output format (little endian), read by common/nds/src/text.c:

    "FNT1", u16 count, u8 ascent, u8 descent, u8[4] reserved
    count x { u32 codepoint, u32 bitmap offset, u8 width, u8 height,
              i8 x offset, i8 y offset (from baseline, up), u8 advance, u8[3] }
    bitmaps: rows of ceil(width / 8) bytes, MSB = leftmost pixel

Glyphs are sorted by codepoint for binary search.
"""
import argparse
import struct
import sys
from pathlib import Path


def parse_bdf(path: Path, wanted: set[int]) -> tuple[dict, int, int]:
    glyphs = {}
    ascent = descent = 0
    cur = None
    rows = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split()
        if not parts:
            continue
        key = parts[0]
        if key == "FONT_ASCENT":
            ascent = int(parts[1])
        elif key == "FONT_DESCENT":
            descent = int(parts[1])
        elif key == "STARTCHAR":
            cur = {}
        elif cur is None:
            continue
        elif key == "ENCODING":
            cur["cp"] = int(parts[1])
        elif key == "DWIDTH":
            cur["adv"] = int(parts[1])
        elif key == "BBX":
            cur["w"], cur["h"], cur["x"], cur["y"] = map(int, parts[1:5])
        elif key == "BITMAP":
            rows = []
        elif key == "ENDCHAR":
            if cur.get("cp") in wanted:
                nbytes = (cur["w"] + 7) // 8
                cur["bits"] = b"".join(int(r, 16).to_bytes(len(r) // 2, "big")[:nbytes] for r in rows)
                glyphs[cur["cp"]] = cur
            cur = rows = None
        elif rows is not None:
            rows.append(key)
    return glyphs, ascent, descent


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bdf")
    ap.add_argument("out")
    ap.add_argument("text", nargs="*", help="UTF-8 files whose characters must be included")
    args = ap.parse_args()

    wanted = set(range(0x20, 0x7F)) | {0x3000}
    for t in args.text:
        wanted |= {ord(c) for c in Path(t).read_text(encoding="utf-8") if ord(c) >= 0x20}

    glyphs, ascent, descent = parse_bdf(Path(args.bdf), wanted)
    missing = sorted(wanted - set(glyphs))
    if missing:
        print(f"mkfont: {len(missing)} characters missing from {args.bdf}: "
              + "".join(chr(c) for c in missing[:40]), file=sys.stderr)

    order = sorted(glyphs)
    header = b"FNT1" + struct.pack("<HBB4x", len(order), ascent, descent)
    table_size = 16 * len(order)
    table = bytearray()
    bitmaps = bytearray()
    for cp in order:
        g = glyphs[cp]
        offset = len(header) + table_size + len(bitmaps)
        table += struct.pack("<IIBBbbB3x", cp, offset, g["w"], g["h"], g["x"], g["y"], g["adv"])
        bitmaps += g["bits"]
    data = header + table + bitmaps
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(data)
    print(f"mkfont: {len(order)} glyphs, {len(data)} bytes -> {args.out}")


if __name__ == "__main__":
    main()
