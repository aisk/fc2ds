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
games/<name>/    one game, in its own git repository (git-ignored here)
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

## Games and copyright

This repository holds only the shared code, tools and documentation. A port is
derived from the original game (its code translated to C, its data formats,
its text), so each game lives in its own repository under `games/<name>`,
which this repository ignores. Keep ports of commercial games private.
ROMs are never committed anywhere: provide your own dump.

The process was worked out on a full port of a commercial MMC3 game, kept
private for that reason. A public example based on a freely licensed homebrew
game is planned.

Contributions to the shared parts are welcome, for example support for more
mappers in `common/vnes` and `common/tools/nesref`. Code or data from
commercial games is not accepted.

See `docs/design.md` for the overall approach.
