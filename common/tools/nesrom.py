"""iNES ROM loader shared by all per-game tools."""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path

PRG_BANK = 0x4000
CHR_BANK = 0x2000

MAPPER_NAMES = {0: "NROM", 1: "MMC1", 2: "UxROM", 3: "CNROM", 4: "MMC3", 7: "AxROM"}


@dataclass
class NesRom:
    path: Path
    mapper: int
    mirroring: str
    battery: bool
    prg: bytes
    chr: bytes
    sha1: str

    @property
    def mapper_name(self) -> str:
        return MAPPER_NAMES.get(self.mapper, f"mapper {self.mapper}")

    def prg_bank(self, index: int, size: int = 0x2000) -> bytes:
        """Return a PRG bank of the given size (MMC3 uses 8 KiB banks)."""
        return self.prg[index * size:(index + 1) * size]


def load(path: str | Path) -> NesRom:
    path = Path(path)
    data = path.read_bytes()
    if data[:4] != b"NES\x1a":
        raise ValueError(f"{path}: not an iNES file")
    prg_size = data[4] * PRG_BANK
    chr_size = data[5] * CHR_BANK
    f6, f7 = data[6], data[7]
    mapper = (f6 >> 4) | (f7 & 0xF0)
    if f6 & 0x08:
        mirroring = "four-screen"
    else:
        mirroring = "vertical" if f6 & 0x01 else "horizontal"
    offset = 16 + (512 if f6 & 0x04 else 0)
    prg = data[offset:offset + prg_size]
    chr_ = data[offset + prg_size:offset + prg_size + chr_size]
    return NesRom(
        path=path,
        mapper=mapper,
        mirroring=mirroring,
        battery=bool(f6 & 0x02),
        prg=prg,
        chr=chr_,
        sha1=hashlib.sha1(data[offset:]).hexdigest(),
    )
