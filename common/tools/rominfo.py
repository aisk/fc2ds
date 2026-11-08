"""Print basic information about an iNES ROM."""
import sys

import nesrom


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit("usage: rominfo.py <rom.nes>")
    rom = nesrom.load(sys.argv[1])
    print(f"file      : {rom.path}")
    print(f"sha1(body): {rom.sha1}")
    print(f"mapper    : {rom.mapper} ({rom.mapper_name})")
    print(f"mirroring : {rom.mirroring}")
    print(f"battery   : {rom.battery}")
    print(f"PRG       : {len(rom.prg) // 1024} KiB")
    print(f"CHR       : {len(rom.chr) // 1024} KiB")


if __name__ == "__main__":
    main()
