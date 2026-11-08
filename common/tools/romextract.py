"""Extract PRG/CHR data from an iNES ROM for the NDS build, verifying its checksum."""
import argparse
import sys
from pathlib import Path

import nesrom


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("rom")
    ap.add_argument("--sha1", help="expected SHA-1 of the ROM body (without header)")
    ap.add_argument("--prg", help="write PRG ROM to this file")
    ap.add_argument("--chr", help="write CHR ROM to this file")
    args = ap.parse_args()

    rom = nesrom.load(args.rom)
    if args.sha1 and rom.sha1 != args.sha1.lower():
        sys.exit(f"{args.rom}: SHA-1 mismatch\n  expected {args.sha1}\n  got      {rom.sha1}")

    for out, data in ((args.prg, rom.prg), (args.chr, rom.chr)):
        if out:
            Path(out).parent.mkdir(parents=True, exist_ok=True)
            Path(out).write_bytes(data)


if __name__ == "__main__":
    main()
