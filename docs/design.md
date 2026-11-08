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
game logic (C, ported per game, port/)
    |  reads/writes the NES RAM, PPU, MMC3 and APU registers
    v
common/vnes: virtual NES state (RAM, MMC3, PPU memory/registers, joypad,
             APU register log, scanline timing, raster log)
    |  vraster: per-line view of a finished frame (scroll, CHR banks, ...)
    v
common/nds: ndsframe (frame -> NDS screen state) -> ndsvideo (hardware)
            nesplatform (frame timing, NMI/IRQ, input, sound stream)
```

- Phase A: game logic writes into a virtual NES, the same model as the original code, and keeps the original RAM layout (`ram.h`, generated from `ram.md` and the notes). Porting stays mechanical and verifiable. There is no CPU emulation and no dot-by-dot PPU: the renderer draws a whole frame from the PPU state at its end plus a log of the raster register writes during the frame.
- Phase B (optional, per game): replace parts with native NDS rendering (wider view, extra layers, bottom-screen UI), move data out of ROM formats into editable assets.
- Audio: the ported sound driver writes the APU registers as the original does, and a software 2A03 (`vapu`) turns them into a mixed stream. Phase B can swap channels for richer instruments without touching the driver.

### The shared NES layer

Everything below is game independent and meant to be reused by every port:

- `common/vnes/vnes.[ch]`: the virtual NES. MMC3 only so far (other mappers go here when a game needs them). Besides the state, it keeps a snapshot of the raster registers at the start of each frame and logs every access to them during the visible lines (`$2000/$2001/$2005/$2006`, `$2002` reads, MMC3 bank select/data and mirroring).
- `common/vnes/vraster.[ch]`: replays that log into 240 `VrasterLine`s: scroll position in the 512x512 nametable plane, PPUCTRL/PPUMASK, CHR banks and mirroring of each line. Mid-frame splits (status bars, parallax, wave effects, CHR switches) come out of this without per-game code.
- `common/vnes/vrender.[ch]`: host software renderer of a frame (screenshots, `nesref --shot`).
- `common/vnes/vapu.[ch]`: software 2A03: pulse, triangle and noise with envelopes, sweep, length/linear counters, the frame counter and the NES mixer curves (no DPCM yet). `Vnes.apu_write` feeds it the register writes as they happen. `nesref --wav FILE` writes the reference machine's sound for listening on the host.
- `common/nds/ndsframe.[ch]`: turns a frame into NDS screen state without touching hardware: a 512x512 text BG holding the nametable plane, per-line scroll, 8x8 OBJs (8x16 split in two), palettes, and a cache that loads the NES CHR tiles in use into 1024 NDS tile slots. `ndsScreenCompose` draws the same state on the host, so the NDS output can be checked without an emulator (`nesref --shot` writes `*-nds.ppm`).
- `common/nds/ndsvideo.[ch]`: the hardware backend. Main and sub engine BG0 + OBJ, per-line scroll by HBlank DMA, window 0 for the lines a screen shows.
- `common/nds/nesplatform.[ch]`: the frame loop. `portWaitNmi()` lets NES time pass line by line, calls the game's IRQ handler when the MMC3 counter fires, shows the frame after line 239 (waiting for the NDS vblank) and then calls the game's NMI handler. Game logic takes no NES time. Sound is a 32768 Hz maxmod stream in manual mode: once per frame it asks `vapu` for the samples played since the last frame, so synthesis runs on the ARM9 between the frames' register writes and needs no locking.
- `common/tools/nesref`: host runner and verifier (section 5, step 7).

What a game provides:

1. `port/`: the ported code, with `GameMain()` (reset + main loop calling `portWaitNmi()` and `portTickEnd()`), `Nmi()` and `Irq()`.
2. `GameScreenLayout()`: which NES lines go to the top screen (cropped to 192, e.g. following the player) and which to the bottom screen (e.g. the status bar below a split). See `games/<name>/port/screen.c`.
3. `src/`: `main()` filling a `NesPlatform` (PRG/CHR from the ROM extracted at build time, the NMI/IRQ handlers, `GameMain`) and calling `nesPlatformRun()`.

Note: RAM names from `ram.h` are macros; include the vnes/ndsframe headers before `port.h` where both are needed, since names like `mirroring` clash with struct fields.

### Renderer TODO

- Screen edge flicker while scrolling: the NES hides the column being streamed under the PPUMASK left-column mask, which the NDS now does too (window 1 covers x 0-7). If attribute glitches remain at the right edge (horizontal mirroring), crop or widen the plane view there.
- Flicker transparency: some objects are drawn every other frame on purpose so that they look half transparent on a CRT (e.g. the spider threads in the first port stage 1). Render them steady with NDS alpha blending instead: either detect sprites that alternate frame by frame in ndsframe, or let the game mark them (per object type).
- Colour: calibrated palette, emphasis bits, smooth fades (section 3).

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
