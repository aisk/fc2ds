# @TITLE@

| | |
|---|---|
| ROM | `roms/@NAME@.nes` (provide your own dump) |
| SHA-1 (without header) | `@SHA1@` |
| Mapper | @MAPPER@ |
| PRG / CHR | @PRG@ KiB / @CHR@ KiB |
| Mirroring | @MIRRORING@ |

## Status

The phases are described in `docs/porting-guide.md`.

- [x] Phase 0: project created (`common/tools/newgame.py`), CHR viewer builds
- [ ] Phase 1: bank layout and disassembly (`disasm/layout.toml`, `make disasm` clean)
- [ ] Phase 2: RAM map and notes (`ram.md`, `notes/*.md`)
- [ ] Phase 3: data formats and viewer (stages, sprites, text)
- [ ] Phase 4: verification harness (`TICK_PC`, first `verify/` scripts)
- [ ] Phase 5: all routines ported and reached by the scripts
- [ ] Phase 6: native (`make verify` with `--native`, no 6502 code left)
- [ ] Phase 7: running on the NDS (screen layout, checked on melonDS)
- [ ] Phase 8: sound
- [ ] Phase 9: translation and subtitles
- [ ] Phase 10: enhancements and fixes (`fixes.md`)

## Next steps

1. Phase 1: find the reset/NMI/IRQ vectors, the bank switch routine and any jump table dispatchers, and describe them in `disasm/layout.toml`.

## Build

- `make`: extracts CHR and PRG from `../../roms/@NAME@.nes` (SHA-1 checked) and builds `build/@NAME@.nds`.
- `make disasm`: traces the code with `disasm/layout.toml` into `build/disasm/` (not committed).

## Technical notes

(Vectors, bank layout, main loop, dispatchers: fill in during phase 1.)
