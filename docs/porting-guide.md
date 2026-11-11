# Porting guide

How a NES game goes from a ROM to a native NDS port in this repository, phase by phase. It is the process the first port (a commercial MMC3 game, kept private) went through, written so that the next game (or the next person, or a new Claude Code session) can follow it. `docs/design.md` explains why things are built this way; this file is about what to do.

Each phase lists what it produces and when it is done. The game's `README.md` keeps a checklist of the phases, so anyone can see where a port stands and pick it up from there. In Claude Code, the `nes-port` skill (`.claude/skills/nes-port`) drives this guide.

## Before you start

- Toolchain: `docs/setup.md` (BlocksDS in WSL, cc65, Python 3 with Pillow and PyYAML). Build in a login shell (`bash -lc make`) so the BlocksDS variables are set.
- ROMs live in `roms/` and are never committed, nor is anything generated from them (disassembly listings, dumped text, extracted graphics). Only hand-written knowledge is committed: layouts, names, notes, ported code, translations.
- What the shared layer supports today (extend it first if the game needs more):
  - Mappers: MMC3, NROM and UxROM (with its bus conflict), in `common/vnes` (the virtual NES) and `common/tools/nesref` (the reference runner). CNROM, MMC1 are small additions there.
  - The sprite 0 hit is modelled (from the OAM and the scroll at the start of the frame), so a status bar split that polls PPUSTATUS works.
  - CHR ROM or 8 KiB of CHR RAM. With CHR RAM the comparison includes its contents, and the NDS renderer converts the tiles the game writes again each frame.
  - The disassembly tracer models PRG as one fixed segment plus pages that switch in one window.
  - NTSC timing; the synthesizer plays DPCM samples, without the DMC IRQ.

## Phase 0: project

Create the game directory from the template:

```sh
python3 common/tools/newgame.py path/to/game.nes <name> --title "Game Title"
cd games/<name> && bash -lc make
```

This copies the ROM to `roms/<name>.nes`, records its SHA-1 (the build refuses any other dump), guesses the bank layout, and sets up a CHR viewer. `common/tools/rominfo.py` prints the header information again at any time.

Done when: `make` builds `build/<name>.nds` and the CHR viewer shows the tiles (on melonDS, or headless with `common/tools/dscheck.py`, see "Checking on the NDS" below).

## Phase 1: disassembly

Goal: a complete, reassembling disassembly that later phases can read. Only the description is committed (`disasm/layout.toml`), the listings are generated into `build/disasm/` by `make disasm`.

1. Find the vectors (NMI, RESET, IRQ at $FFFA-$FFFF of the fixed bank) and read the reset code: how it maps banks, where the main loop is.
2. Find the bank switch routine (its address goes in `bank_switch`; the tracer follows the page that `lda #n / jsr bank_switch` maps) and fix `[layout]` if the guess from the mapper is wrong.
3. Find jump table dispatchers (`jsr Dispatch` followed by `.word` targets) and list them in `dispatchers`.
   The routine may take the page in another register (`bank_switch_reg`), there may be several (`bank_switch` takes a list), and routines that always map one page go in `bank_calls`.
4. Run `make disasm` and read `build/disasm/report.txt`: it lists indirect jumps and data the tracer could not resolve. Add `table` entries (pointer tables reached some other way), `entry` points (unreferenced routines) and `[indirect]` notes until the report is clean. A vector the hardware never takes (the IRQ of a mapper without IRQs) may point at data: list it in `unused_vectors` (`[layout]`). The tracer already knows that a branch right after `lda/ldx/ldy #imm`, `clc`, `sec` or `clv` can be unconditional. Other branches that are always taken go in `taken`. Code pointers kept as two tables of low and high bytes are a `table` with `hi`; a table of structures that start with N code pointers is `depth = 2` with `inner = N` (null pointers are skipped in tables with a `count`). Routines that read data bytes after their `jsr` go in `inline_args`, illegal opcodes the game runs on purpose as debug traps in `traps`, and jumps into code the game writes to RAM are explained in `[indirect]`.

If someone has published an annotated disassembly of the game, use it: clone it into the game's `ref/` (git-ignored, never committed), find the address of each of its labels, and generate the name tables (`ram.md`, a routines file) from it. With the routines file in `entries_from`, every routine of the reference is an entry point and the trace is complete at once. Keep its documentation as reading material; `notes/` then only needs what the port uses.

Done when: `make disasm` reports that all listings reassemble to the original ROM and `report.txt` has nothing unexplained. Write the vectors, bank layout, main loop and dispatchers into the README's technical notes.

## Phase 2: RAM map and notes

Goal: names for the RAM the game uses, and notes on how its systems work.

- `ram.md`: one table row per variable (`| $00A0 | player_x | ... |`). Names from `ram.md` and every markdown file listed in `names_from` (layout.toml) and `RAM_NAMES` (Makefile) are used in the listings and become C macros in `build/host/ram.h` (`common/tools/mkramh.py`), so the port code reads like the notes.
- `notes/*.md`: one file per system (objects, level format, sound driver, text). Write down data formats with addresses, state machines, and anything odd.
- Odd behaviour that looks like a bug goes into `fixes.md` with its address. Do not fix anything yet: the port must first match the original.

Done when: the main loop, the object system, the level data and the sound driver are described well enough to write decoders.

## Phase 3: data and viewer

Goal: prove the understanding of the data by showing it.

- Decoders for level maps, metatiles, metasprites, palettes and text go in `games/<name>/tools/` (or `common/tools/` if shared).
- The NDS viewer (`src/main.c`) grows from the CHR viewer into a stage, sprite and text viewer, built from the ROM at build time. Keep it reachable in the finished game, for example by holding SELECT at power on.
- Text: a dump tool writes the original text to `build/text/ja.yaml` (not committed); translations go in `text/<lang>.yaml` with the same keys (`docs/design.md`, "Localisation"). Set `TEXT_LANGS` and `TEXT_BASE` in the Makefile.

Done when: every stage renders correctly in the viewer and the text dump reads correctly.

## Phase 4: verification harness

Goal: `make verify` runs the original on the host and compares it with the port after every pass of the main loop ("tick").

1. In the Makefile set:
   - `TICK_PC`: the address where one pass of the main loop ends. Pick a point after the game logic and before anything that runs on its own timing. For example right after the game logic, before the sound driver.
   - `RAM_NAMES`: the markdown files with RAM names.
   - `VERIFY_FLAGS`:
     - `--ignore 180-1FF` leaves the stack out of the comparison.
     - `--idle PC:ADDR` tells the runner where the main loop waits for the NMI, so it can skip ahead.
     - Games that run everything in the NMI and idle in an empty loop: `TICK_PC` is that loop and `--tick-nmi` ends a tick there only after an NMI; `--idle PC` (no address) skips ahead in the loop.
2. Write input scripts in `verify/*.txt`:
   - `TICK BUTTONS` lines, with buttons from `ABsSUDLR` or `-`.
   - `TICK poke ADDR VALUE` jumps straight to a stage or a state.
   - `TICK end` stops the script.
   - Generate scripts when there are many similar cases (for example a small Python script that writes one script per stage).
3. Start porting with something small and central, such as the NMI handler or the bank switch, and register it with `PORT_HOOK`. `make verify` runs in `VERIFY_MODE=--compare` until phase 6: the port machine runs the original code with the ported routines hooked in.

Pitfalls seen so far:
- **Busy waits**: ported code takes no NES time, so a loop polling PPUSTATUS (sprite 0 hit) must call `vnesWaitStatus(mask, value)` to let the lines pass.
- **Speed**: ported routines take no CPU time while the original does, so the original lags in heavy scenes and the two drift apart. Verify runs with `--speed 32`, which makes the reference CPU fast enough never to lag.
- **Input**: comparison and input are per tick, not per video frame, for the same reason.

Done when: a script runs to its end with the first hooks in place, and `make verify` prints the coverage list.

## Phase 5: port the code

Goal: every routine of the original exists in C under `port/`, each one checked against the original.

Conventions:

- One C function per original routine, named after its label, with the original address in a comment. Keep the NES RAM layout and use the `ram.h` names. ROM data and hardware go through `rd()`/`wr()`.
- Scratch RAM is written exactly like the original, since later code may read it. Registers and flags that callers use after a routine are set by its hook wrapper.
- `jsr JumpTable` + inline `.word` list becomes `switch (JumpTable(table, index))`; the helper (`port/port.h`) leaves RAM and registers like the original.
- Routines that pull their caller's return address (`pla/pla`) return a flag, and the caller returns at once.
- Calls to routines that are not ported yet go through `call6502(addr)`, so files can be ported in any order. They become direct calls before phase 6.
- Each file ends with an `#ifdef NESREF` block of `PORT_HOOK(addr, bank, h_Name)` lines. Use `PAGE_BANK(page, addr)` for routines in switchable pages.
- Bugs of the original stay in the port (noted in `fixes.md`). NES limits stay too: the port must match the original, and the NDS side removes the limits later.
- Routines that only C code calls need no hook: they are verified through their callers. `make verify` lists ported routines whose original code no script reaches; those are not verified yet.

How the work is split:

- One file (or a few) per PRG page or address range: `p0a_objects.c`, `p1b_modes.c` and so on. Shared helpers (`objutil.c`, `ppuqueue.c`) are ported first, since everything calls them.
- Independent ranges can be ported in parallel by agents in separate git worktrees, each owning its files and adding verify scripts for the code it ported. Merge one at a time and rerun `make verify` after each merge. Two agents adding the same helper is the usual merge conflict.
- After each step `make verify` must pass, and the coverage list shows which ported routines no script reaches yet. Add scripts until only dead code is left (code with no callers, listed in the README).

Done when: every routine is ported, `make verify` passes, and the routines it does not reach are known dead code.

## Phase 6: native

Goal: the port runs with no 6502 code at all, exactly as it will on the NDS.

1. Port what is left of the fixed bank: interrupt handlers and the IRQ split handlers, the reset code (`Reset`), and the main loop (`GameMain`).
   - The main loop calls `portWaitNmi()` where the original waits for the NMI, and `portTickEnd()` where a tick ends.
   - If the original leaves something in a register that the next pass reads, keep it in a variable. For example, a variable that keeps X of the idle loop.
2. Replace every `call6502` and `call_regs` outside the `NESREF` blocks with direct calls. Or keep them: compiled with `PORT_REGS` (in the game's `DEFINES`), the port calls routines through their `PORT_HOOK` wrappers on the NDS too (`common/nds/src/porthooks.c`), the same dispatch `--native` uses on the host. This suits code where registers and flags cross routine boundaries everywhere, since the wrappers `make verify` checked become the calling convention.
3. Switch `VERIFY_MODE` to `--native`. The game's `GameMain` now drives the port machine: interrupts call the C handlers, and any 6502 instruction the port would still run is an error. `--pcs FILE` lists such instructions while you get there.
4. `port/` must compile for the NDS without warnings (`make`).

Done when: `make verify` passes in native mode for every script.

## Phase 7: on the NDS

Goal: the game runs on the NDS through the shared layer (`docs/design.md` section 4).

1. `src/game.c` fills a `NesPlatform` (PRG and CHR from `rom_data.s`, `Nmi`, `Irq`, `GameMain`) and calls `nesPlatformRun()`. Keep the viewer reachable, for example by holding SELECT at power on.
2. Write `GameScreenLayout()` (in `port/screen.c`): which NES lines go to the 192-line top screen and which to the bottom screen.
   - A status bar under an IRQ split is detected from the per-line CHR banks and moved to the bottom screen.
   - The top screen follows the player.
3. Check on the host first:
   - `--shot TICK:PREFIX` writes the NES frame (`-ref.ppm`, `-port.ppm`) and both NDS screens as the renderer lays them out (`-port-nds.ppm`).
   - The NDS screens must match the NES frame line for line inside the shown area.
4. Then on the NDS:
   - Headless with `dscheck.py --shake --load`.
   - Then ask the user to check on melonDS.

Pitfalls seen so far:
- **Frame budget**: a frame that misses the NDS vblank shows a broken picture. The scroll HBlank DMA is now restarted every vblank, so such a frame only repeats the last one. Still keep the load under 100%: hold L+R in game to see it, or use `dscheck.py --load`. The usual cost centres (sound synthesis, `ndsScreenBuild`) already run from ITCM as ARM code.
- **Nametable switching**: raster effects that switch nametables line by line need the map built per cell, not per row. This is done in `ndsframe.c`.
- **Left column**: games hide the column they are redrawing with the PPUMASK left-column bits, and the NDS must do the same. This is done in `ndsvideo.c`.

Done when: the user has played the title, the first stage, a shop or menu and a boss on melonDS without problems. Collect their checks in `REVIEW.md` instead of blocking on them (see "Working with Claude Code").

## Phase 8: sound

Goal: the original music and effects. Once the sound driver is ported and verified (the comparison includes every APU write, in order), sound needs no game code: `common/vnes/vapu` synthesizes the APU writes, and `nesplatform` streams them through maxmod.

- Listen on the host: `--wav FILE` writes the run's sound.
- DPCM samples play from the CPU bus as the game maps it (`vapuSetMemory`). The DMC IRQ is not modelled: a game that uses it for timing needs it added to `vapu.c` and `vnes`.

Done when: the user has listened on melonDS.

## Phase 9: translation

Goal: the game's dialogue in other languages, without changing how the game draws its text.

- The game keeps drawing its original text, and the translation appears as a subtitle on the bottom screen (`common/nds/subtitle.[ch]`).
- The port calls `portMessage(id)` where the game starts printing message `id`, and `portMessage(-1)` where the screen changes (for example the routine that prints a message and the one that changes screens).
- `src/game.c` lists the languages and picks one from the firmware language. Touching the subtitle band cycles through the languages and off.
- UI strings that are not in the message tables (language names) need their characters in `TEXT_EXTRA`, since the fonts are cut down to the characters in use.
- Text the game prints through other routines (credits, staff roll) needs its own hook.

Done when: every message shows its translation, and the translated names in `text/*.yaml` are confirmed by the user.

## Phase 10: enhancements and fixes

- **Bug fixes**: go through `fixes.md`. Each real bug becomes a `FIX_<NAME>` switch, on by default, so `make verify` can still be run with the original behaviour.
- **Enhancements**: hardware limits are removed where they are (`docs/design.md` section 3).
- **Own bottom screen**: instead of the NES status bar, draw a HUD into `common/nds/subscreen.h` from `NesPlatform.frame` (in `src/hud.c`).
  - Read NES RAM only, and redraw a part only when its value changed. Draw at most one part per frame, and long work (a stage map) over several frames, since everything here adds to the frame's load.
  - Icons come straight from the CHR the status bar uses (`textBlitNes`), numbers and labels from the 12 px font (their characters go in `TEXT_EXTRA`).
  - `GameScreenLayout()` gets a switch to leave the status bar out. Keep a way back to the NES status bar (for example touching the HUD), and show it whenever the game uses the status bar for its own UI (for example a weapon select or pause screen).
  - The 8 sprites per line limit and slowdown are already gone, by design.
  - Games that cap their own sprites at the 64 of OAM (dropping the rest, rotating OAM so the dropped ones flicker) can write the sprites past 64 into `vnes->oam_extra` behind a switch: the renderer draws up to 128.
  - Flicker that the game's own code makes on purpose (OAM rotation, objects drawn every other frame for transparency) needs a switch in the port plus, where the NES has no way to say it (alpha, more colours), extra fields that the C code writes for the renderer. The renderer TODO in `docs/design.md` lists what is open.

## Checking on the NDS

`common/tools/dscheck.py` runs a build in DeSmuME without a window. It needs py-desmume (`docs/setup.md`).

- **Input**: a frame-numbered script (keys, touch, pokes into NES RAM by ELF symbol).
- **Screenshots**: `--shots`.
- **Shake**: `--shake` finds frames where a whole screen moved vertically.
- **Load**: `--load` reads the frame load the platform measures.

DeSmuME does not model the ARM9 caches, so its load is an upper bound. melonDS and hardware are lighter. The final check is always the user on melonDS.

## Working with Claude Code

- Project rules: code, comments and commit messages in English; commits have a gitmoji subject and no body; never commit ROMs or ROM-derived files. A local `CLAUDE.md` can add personal rules (it is not part of the repository).
- Items that need the user (melonDS checks, listening, translation choices) go to `games/<name>/REVIEW.md`. That file is not committed (add it to `.git/info/exclude`). Keep working instead of blocking on them, and go through the list with the user at the end.
- The game README's status checklist and "Next steps" are the hand-over between sessions: update them with every commit that finishes a step.
