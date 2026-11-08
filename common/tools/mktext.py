"""Convert a language's message YAML into the NDS runtime format.

Input: a YAML file with a `messages:` mapping. Each message is either
`{same_as: key}` or has `lines:`, a list of `[x, y, "text"]` or plain strings.
Plain strings take their position from the same line of the base language
(the Japanese text dumped from the ROM), so translations only need positions
where the layout differs. Messages missing from the translation fall back to
the base language with a warning.

Output (little endian), read by common/nds/src/text.c:

    "TXT1", u16 count, u16 reserved, u32 offsets[count]
    message: u8 line count, then per line u8 x, u8 y, UTF-8 text, 0

Also writes `<out>.chars`, the UTF-8 text of every line, for mkfont.py.
"""
import argparse
import struct
import sys
from pathlib import Path

import yaml


def load(path: Path) -> dict:
    return yaml.safe_load(path.read_text(encoding="utf-8")).get("messages", {})


def final_key(msgs: dict, key: str) -> str:
    seen = set()
    while isinstance(msgs.get(key), dict) and "same_as" in msgs[key]:
        if key in seen:
            raise SystemExit(f"mktext: same_as loop at {key}")
        seen.add(key)
        key = msgs[key]["same_as"]
    return key


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lang", help="YAML of the language to build")
    ap.add_argument("out")
    ap.add_argument("--base", help="YAML of the base language (positions, fallback)")
    args = ap.parse_args()

    msgs = load(Path(args.lang))
    base = load(Path(args.base)) if args.base else msgs
    keys = sorted(base)

    blobs = []
    chars = []
    for key in keys:
        # A translation follows the base language's same_as links unless it
        # defines the message itself.
        bkey = final_key(base, key)
        b = base.get(bkey) or {"lines": []}
        mkey = final_key(msgs, key if key in msgs else bkey)
        m = msgs.get(mkey)
        if m is None:
            if b.get("lines"):
                print(f"mktext: {args.lang}: {key} missing, using base text", file=sys.stderr)
            m = b
        base_lines = b.get("lines") or []
        lines = []
        for i, ln in enumerate(m.get("lines") or []):
            if isinstance(ln, str):
                if i < len(base_lines):
                    x, y = base_lines[i][0], base_lines[i][1]
                else:
                    x, y = base_lines[-1][0], base_lines[-1][1] + 2 * (i - len(base_lines) + 1)
                ln = [x, y, ln]
            lines.append(ln)
        blob = bytearray([len(lines)])
        for x, y, text in lines:
            blob += bytes([x, y]) + text.encode("utf-8") + b"\0"
            chars.append(text)
        blobs.append(bytes(blob))

    head = b"TXT1" + struct.pack("<HH", len(blobs), 0)
    offsets = []
    pos = len(head) + 4 * len(blobs)
    for b in blobs:
        offsets.append(pos)
        pos += len(b)
    data = head + struct.pack(f"<{len(offsets)}I", *offsets) + b"".join(blobs)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    Path(str(out) + ".chars").write_text("\n".join(chars) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
