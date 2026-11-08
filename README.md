# fc2ds

<img src="https://pbs.twimg.com/media/HWnUrsGbEAAsKuO?format=png&name=large" alt="A port running on melonDS" align="right" width="40%">

English | [中文](README.zh-CN.md)

Reverse-engineer Famicom/NES games and reimplement them natively on Nintendo DS.

This project is for learning only and meant for non-commercial games such as
homebrew. Only reverse-engineer a commercial game if you own a legal copy of
it, and do not publish the results. Any infringement involving a game
reverse-engineered with these tools is the responsibility of whoever did it,
not of this repository; please take it up with them.

The repository contains no commercial game and no proprietary console code
(no BIOS, firmware or official SDK; the NDS side is built with the open
source BlocksDS toolchain). It holds only generic code: tools, a runtime and
documentation. It does not reverse-engineer anything by itself: the actual
work, such as rewriting a game's assembly in C, is done by the person doing
the port.

## Why the NDS

An NDS screen is 256 pixels wide like the NES, and a bit shorter (192 lines
against the 224 or so a TV shows). Almost every NES game keeps a status bar
with HP, money and the like at the top or bottom of the screen. On the NDS it
moves to the second screen, and the play area fits the top screen as it is.

The bottom screen can also be redrawn by the port: it reads the game's state
from RAM and shows the status bar again with the original icons, plus what the
NES had no room for, such as a stage map, boss HP, or the dialogue translated
into other languages (`common/nds/subscreen`). The original status bar is one
touch away.

## How it works

Not an emulator. The game's 6502 code is decompiled routine by routine into
equivalent C and compiled to native ARM code. `make verify` runs it side by
side with the original on the host and checks RAM, PPU and APU state every
frame.

The C code still drives NES hardware, so a small virtual NES
(`common/vnes`) provides the PPU (graphics), APU (sound) and mapper:

- **Graphics** are drawn by the NDS 2D hardware, not in software. NES tiles,
  nametables, sprites and palettes map onto NDS tiles, BG maps, OBJs and
  palette RAM; mid-frame scroll splits become per-line scroll by HBlank DMA.
  No 8 sprites per line limit, no flicker.
- **Sound** is a software 2A03 on the ARM9, streamed through maxmod.

Details: `docs/design.md`.

## Porting a game

This is not a one-click decompiler. The tools here trace, verify and run the
code, but reading the disassembly and translating it to C is done by hand or
with an LLM coding agent such as Claude Code, which the repository is set up
for: `CLAUDE.md` and the skills in `.claude/skills/` guide it through the
process.

```sh
python3 common/tools/newgame.py path/to/game.nes <name> --title "Game Title"
cd games/<name> && bash -lc make
```

Then follow `docs/porting-guide.md`. In Claude Code, ask it to port the game:
the `nes-port` skill walks through the phases and `nds-check` runs the NDS
build headless. Setup: `docs/setup.md`.

## Games and copyright

Ports of freely licensed games, such as homebrew whose license allows it, are
welcome here under `games/<name>`. A port of a commercial game is derived from
that game, so keep it in a private repository of its own: `games/` is ignored
by default, and `newgame.py` starts each game as a separate git repository
(`--here` to keep it in this one). ROMs are never committed.

Contributions to the shared parts, such as more mappers, are welcome too.

## TODO

- [x] Mapper: MMC3 (mapper 4)
- [ ] Mapper: NROM (mapper 0)
- [ ] Mapper: MMC1 (mapper 1)
- [ ] Mapper: UxROM (mapper 2)
- [ ] Mapper: CNROM (mapper 3)
- [ ] CHR RAM games
- [ ] Sound: DPCM channel
- [ ] Sound: play pulse and noise on the NDS PSG channels (ARM7) instead of the ARM9 synth
- [ ] Graphics: draw the column past the right edge instead of cropping 8 px
- [ ] Graphics: steady alpha blending for sprites a game flickers for transparency
- [ ] Graphics: calibrated palette and smooth fades
- [ ] Bottom screen: shared HUD parts (bars, icon rows, stage map)
- [ ] A public example port of a freely licensed homebrew game

## License

[MPL-2.0](LICENSE). Changes to the files of this repository stay open, while a
game built on them can use any license. It covers this repository only, not
the games ported with it.
