"""Create games/<name> for a new ROM from common/template (porting phase 0).

    newgame.py ROM NAME [--title TITLE]

Copies the ROM to roms/NAME.nes (git-ignored), fills in its checksum, mapper
and a first guess of the bank layout, and sets up a CHR viewer that builds
with `make`. See docs/porting-guide.md for what comes next.
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

import nesrom

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "common" / "template"
# Mappers the shared NES layer (common/vnes, nesref) can run today.
SUPPORTED = {4: "MMC3"}


def layout_guess(rom: nesrom.NesRom) -> str:
    prg = len(rom.prg)
    if rom.mapper == 0 or prg <= 0x8000:
        # NROM: no banking, one fixed segment.
        return (f"window = 0x8000\npage_size = 0x4000\npages = 0\n"
                f"fixed_prg = 0x0\nfixed_base = {0x10000 - prg:#x}\nfixed_size = {prg:#x}")
    # Banked: last 16 KiB fixed at $C000, the rest as 16 KiB pages at $8000.
    return (f"window = 0x8000\npage_size = 0x4000\npages = {prg // 0x4000 - 1}\n"
            f"fixed_prg = {prg - 0x4000:#x}\nfixed_base = 0xC000\nfixed_size = 0x4000")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("name", help="directory name under games/ and symbol prefix (lowercase, a-z0-9_)")
    ap.add_argument("--title", help="game title for the NDS banner and README")
    ap.add_argument("--here", action="store_true",
                    help="keep the game in this repository (freely licensed games only) "
                         "instead of starting a git repository of its own")
    args = ap.parse_args()

    if not re.fullmatch(r"[a-z][a-z0-9_]*", args.name):
        sys.exit("name must be lowercase letters, digits and _, starting with a letter")
    dest = ROOT / "games" / args.name
    if dest.exists():
        sys.exit(f"{dest} already exists")
    rom = nesrom.load(args.rom)
    if not rom.chr:
        print("warning: CHR RAM game (no CHR ROM): the CHR viewer shows nothing and "
              "common/vnes needs CHR RAM support before phase 4", file=sys.stderr)
    if rom.mapper not in SUPPORTED:
        print(f"warning: mapper {rom.mapper} ({rom.mapper_name}) is not supported by common/vnes "
              f"and nesref yet (only {', '.join(SUPPORTED.values())}); add it before phase 4",
              file=sys.stderr)

    roms = ROOT / "roms"
    roms.mkdir(exist_ok=True)
    target = roms / f"{args.name}.nes"
    if Path(args.rom).resolve() != target.resolve():
        shutil.copyfile(args.rom, target)

    values = {
        "@NAME@": args.name,
        "@TITLE@": args.title or args.name,
        "@SHA1@": rom.sha1,
        "@MAPPER@": f"{rom.mapper} ({rom.mapper_name})",
        "@PRG@": str(len(rom.prg) // 1024),
        "@CHR@": str(len(rom.chr) // 1024),
        "@MIRRORING@": rom.mirroring,
        "@LAYOUT@": layout_guess(rom),
    }
    for src in TEMPLATE.rglob("*"):
        if src.is_dir():
            continue
        out = dest / src.relative_to(TEMPLATE)
        out.parent.mkdir(parents=True, exist_ok=True)
        text = src.read_text()
        for k, v in values.items():
            text = text.replace(k, v)
        out.write_text(text)
    # A port is derived from its game, so by default it is a repository of
    # its own, kept out of this one (see README.md).
    if args.here:
        print(f"add '!games/{args.name}/' to .gitignore to commit it here")
    else:
        subprocess.run(["git", "init", "-q", str(dest)], check=False)
    print(f"created {dest.relative_to(ROOT)} for {values['@MAPPER@']}, "
          f"PRG {values['@PRG@']} KiB, CHR {values['@CHR@']} KiB")
    print(f"next: cd {dest.relative_to(ROOT)} && bash -lc make")


if __name__ == "__main__":
    main()
