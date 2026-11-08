# fc2ds

Reverse-engineer Famicom/NES games and reimplement them natively on Nintendo DS.

## Porting a game

Put the ROM somewhere, then follow `docs/porting-guide.md`:

```sh
python3 common/tools/newgame.py path/to/game.nes <name> --title "Game Title"
cd games/<name> && bash -lc make
```

In Claude Code, the `nes-port` skill (`.claude/skills/`) walks through the same phases, and `nds-check` runs the NDS build headless. Setup: `docs/setup.md`.

## Layout

```
roms/            original ROMs (git-ignored, provide your own dump)
common/
  tools/         host tools: ROM parsing, disassembly tracer, text and font tools,
                 nesref (reference runner / verifier), newgame.py, dscheck.py
  vnes/          virtual NES the ported code runs on (MMC3, PPU state, raster log, APU synth)
  nds/           NDS runtime: frame mapping, video, platform loop, sound stream, text, subtitles
  template/      starting point of a new game (newgame.py)
games/<name>/
  README.md      status checklist, next steps, technical notes
  disasm/        layout.toml for the tracer (listings are generated, not committed)
  ram.md notes/  RAM map and notes on the game's systems
  port/          the game's code in C, checked against the original by `make verify`
  verify/        input scripts for `make verify`
  src/           NDS entry point and viewer
  text/          translations
  fixes.md       bugs of the original and their FIX_ switches
docs/            design, porting guide, setup
```

## Games

| Directory | Game | Mapper | Status |
|---|---|---|---|
| `games/<name>` | (private) | MMC3 | playable with sound and subtitles |

See `docs/design.md` for the overall approach.
