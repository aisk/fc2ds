# fc2ds

Reverse-engineer Famicom/NES games and reimplement them natively on Nintendo DS.

## Layout

```
roms/            original ROMs (git-ignored, provide your own dump)
common/
  tools/         Python tools shared by all games (ROM parsing, CHR/map extraction, text tools)
  nds/           shared NDS runtime (NES-style PPU layer on NDS VRAM, input, sound, i18n, dual-screen HUD)
games/<name>/
  README.md      game notes, ROM checksum, progress
  disasm/        annotated disassembly
  src/           NDS reimplementation of the game logic
  text/          translations (zh / ja / en)
  fixes.md       list of original bugs and how they are fixed
docs/            design notes shared by all games
build/           generated output (git-ignored)
```

## Games

| Directory | Game | Mapper |
|---|---|---|
| `games/<name>` | (private) | MMC3 |

See `docs/design.md` for the overall approach.
