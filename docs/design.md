# Design: NES game -> native NDS reimplementation

## 1. Goals

- Reimplement game logic in C, running natively on the ARM9 (no 6502 emulation at runtime).
- An enhanced port, not a 1:1 copy. Game rules, physics and timing stay the same, but limits that come from the NES hardware are removed (see section 3).
- Use the second screen for HUD / status / map so the top screen is pure play area.
- Text in Simplified Chinese, Japanese and English, selectable at runtime.
- Fix serious original bugs, each behind a compile-time switch.
- Never ship or commit ROM data. Assets are extracted from the user's own ROM at build time.

## 2. Screen mapping

| | NES | NDS (per screen) |
|---|---|---|
| Resolution | 256x240 (about 224 visible on NTSC) | 256x192 |
| BG | 1 layer, 8x8 tiles, 4 palettes x 3 colours | 4 layers, 256 colours or 16x16 palettes |
| Sprites | 64, 8 per scanline | 128, no per-line limit |

- Width is the same (256), so horizontal mapping is 1:1.
- Height: remove the in-game status bar (usually 24-40 px), which then goes to the bottom screen. If the play area is still above 192 px, the camera follows the player vertically (dead zone + clamp). We do not scale, because scaling blurs pixel art.
- No sprite flicker. The NES 8-sprites-per-line limit does not exist on NDS, so the OAM rotation that games use to spread the flicker is dropped.

## 3. Hardware limits we remove

The port keeps what the player experiences as game design, and drops what was only a workaround for the NES. These are not switchable, they are simply how the port works.

- **Slowdown**: the original drops frames when the CPU cannot finish a game tick in one frame. The port always runs one tick per frame.
- **Sprite flicker and dropout**: all sprites are drawn every frame, without OAM cycling.
- **Object limits** that exist only to save CPU time or OAM slots can be raised, as long as gameplay does not change (e.g. a fixed spawn table still spawns the same enemies).
- **Raster tricks** (MMC3 IRQ splits for status bars, mid-frame CHR switches) become separate NDS layers or the bottom screen.
- **Missing visuals at screen edges** (attribute glitches from horizontal mirroring, the hidden left 8 px column) are drawn correctly.

Colour is also open to enhancement, as long as it stays faithful to the original look:

- **Palette**: NES colour indices ($00-$3F) map to RGB15 through one shared, well-calibrated table in `common/nds`, instead of whatever an emulator happens to use. Emphasis and greyscale bits ($2001) are applied properly.
- **Fades**: the NES fades by stepping palette indices in a few coarse steps. The port can use NDS master brightness or blending for smooth fades and flashes.
- **Attribute clash**: the NES picks a palette per 16x16 area and 3 colours per tile. NDS allows a palette per 8x8 tile and 15 colours per palette, so places where the original art had to compromise can be fixed by per-game overrides.
- **Layered sprites**: games often stack two sprites to get more than 3 colours. The port can merge them into one 4bpp sprite, which also removes the flicker the stacking caused.

Visual enhancements never change collision or timing. Larger art changes (redrawn tiles) are per-game assets and stay optional.

Anything that changes gameplay itself (difficulty, damage, item behaviour) is not an enhancement but a bug fix, and goes through `FIX_xxx` (section 8).

## 4. Architecture

```
game logic (C, ported per game)  ->  virtual NES PPU state (nametables, OAM, palettes, scroll)
                                            |
                common/nds renderer  ------>  NDS BG0/BG1 + OAM (top screen)
                                            \->  HUD/map/menus (bottom screen, native NDS)
```

- Phase A: game logic writes into a virtual NES (`common/vnes`: RAM, MMC3, PPU memory and registers, joypad), the same model as the original code, and keeps the original RAM layout (`ram.h`, generated from `ram.md` and the notes). The renderer converts the PPU state to NDS hardware every vblank. Porting stays mechanical and verifiable.
- Phase B (optional, per game): replace parts with native NDS rendering (wider view, extra layers, bottom-screen UI).
- CHR banks (MMC3 1K/2K switching) map to NDS VRAM tile slots. The full 128 KiB CHR fits in VRAM, so bank switching becomes a tile index offset.
- Audio: port the game's sound driver data and play it through a 2A03 synth on ARM7 (square/triangle/noise/DPCM in PSG channels or a mixed stream).

## 5. Workflow per game

1. **Identify**: `rominfo.py`, mapper, bank layout.
2. **Dump graphics**: `chrdump.py`, then palettes and nametable captures from an emulator.
3. **Disassemble**: auto-split PRG banks, disassemble with da65 (cc65), then annotate in Mesen's debugger (labels exported back to the repo).
4. **Map RAM**: zero page and $0300-$07FF variables, documented in `games/<name>/ram.md`.
5. **Decode data formats**: levels, metatiles, enemy tables, text and its encoding table. Write extractors in `common/tools` or `games/<name>/tools`.
6. **Port logic**: routine by routine into C. Keep the original address in a comment (`/* $C3A0 */`) for traceability.
7. **Verify**: `make verify` builds `common/tools/nesref` on the host, a headless 6502 + MMC3 runner on top of `common/vnes`. It runs two machines in lockstep with the input scripts in `games/<name>/verify/`: one runs the original code, the other the port. While a game is being ported (`VERIFY_MODE=--compare`), the port machine runs the original code with every ported routine hooked in (`PORT_HOOK` in each port file), so a routine can be checked as soon as it is ported. Once everything is ported, `--native` runs the port exactly as on the NDS: its C main loop drives the machine, interrupts call the C handlers, and no 6502 code runs. After every tick, RAM, PPU memory, PPU/MMC3 registers and APU writes must match. Compare per tick (one pass of the main loop), not per video frame: input is applied per tick, and the CPU runs faster than real hardware so the original does not lag. Mesen is still useful for exploring the original, but not needed for verification.
8. **Improve**: HUD on the bottom screen, translations, bug fixes (`fixes.md`).

## 6. Toolchain

| Purpose | Tool | Where |
|---|---|---|
| NDS build | BlocksDS (Wonderful toolchain GCC, libnds-compatible, grit, ndstool, mmutil) | WSL Debian: `/opt/wonderful`, SDK at `/opt/blocksds/core` |
| Asset pipeline | Python 3 + Pillow | Windows or WSL |
| Disassembler | cc65 `da65` (with info files per bank) | WSL: `apt install cc65` |
| Debugging the original | Mesen 2 (debugger, trace logger, Lua scripting, memory diff) | Windows |
| Testing the port | melonDS (accurate, has a GDB stub), DeSmuME | Windows |
| Optional | Ghidra + 6502 support for cross-reference browsing | Windows |

Builds run inside WSL (Debian) with BlocksDS (devkitPro's servers are not reachable from this network). Install script: see `docs/setup.md`.

## 7. Localisation

- The original Japanese text is dumped from the ROM at build time (`build/text/ja.yaml`, not committed). Translations live in `games/<name>/text/{en,zh}.yaml` with the same keys; a plain string line takes its position from the Japanese line, and missing messages fall back to Japanese.
- `common/tools/mktext.py` turns each language into a binary message table, and `common/tools/mkfont.py` cuts a font down to the glyphs that table uses. The font is Fusion Pixel 12px proportional (OFL, fetched by `common/fonts/fetch.sh`): the ja variant for Japanese, zh_hans for Chinese and English. `common/nds/src/text.c` draws it into a tile canvas.
- Text boxes are re-laid out for each language. If space is short, longer text can go on the bottom screen.

## 8. Bug fixes

- Every fix is a `#if FIX_<NAME>` block in the code and is listed in `games/<name>/fixes.md`.
- Default build: all serious fixes on (softlocks, crashes, save/progress corruption). An "original" build turns them off, so the original behaviour can still be checked during verification.
