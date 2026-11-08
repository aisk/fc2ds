---
name: nes-port
description: Port a NES game in this repository to a native NDS game, or continue a port in progress. Use when the user runs /nes-port, drops a ROM into roms/, names a game to port, or asks to continue, check the status of, or take the next step of a port (games/<name>).
---

# NES to NDS port

The process is `docs/porting-guide.md`. Read it first, then `docs/design.md` (the why), and for a game in progress its `games/<name>/README.md`. The README's status checklist says which phase it is in, and its "Next steps" section says what is next.

## 1. Find the game and the phase

Look for work: `.nes` files in `roms/` (or a path the user gave) and game directories in `games/`. A ROM whose name has no `games/<name>` yet is a new game.

- **No ROM and no game**: tell the user to put their ROM into `roms/` and run `/nes-port` again. Stop there.
- **New ROM**: first the confirmation below, then:
  1. Run `python3 common/tools/rominfo.py ROM`.
  2. Pick a short lowercase name (a-z, 0-9, `_`) and the title yourself, from the file name and what you know of the game, and tell the user. Do not wait for approval.
  3. Run `python3 common/tools/newgame.py ROM NAME --title "..."` (it moves the ROM to `roms/NAME.nes`, or copies it from outside `roms/`, and runs `git init` in `games/NAME`). For a freely licensed game the user wants in this repository, add `--here`.
  4. Add the confirmation to `games/NAME/README.md`, under the title: "Ported from a legally owned copy, for personal learning only; not to be published (confirmed by the user on YYYY-MM-DD)."
  5. Check the warnings newgame.py prints. An unsupported mapper or CHR RAM means extending `common/vnes` and `common/tools/nesref` before phase 4. Say so early, and start phases 1-3 anyway.
  6. Go on with phase 1 without waiting.
- **Existing game**: read its README checklist and "Next steps". Continue with the first phase that is not done.
- If several games exist and the user did not name one, ask which.

### Confirmation before a new ROM

Before running anything on a new ROM (rominfo included), ask the user once, with the question tool if there is one:

> To port `<file>`, please confirm: you own a legal copy of this game (dumped from your own cartridge, or a game whose license allows it, such as free homebrew), the port is only for your own learning, and you will not publish the port or anything derived from the ROM.

Options: "I confirm" and "No". On anything but a clear confirmation, explain why it is needed and stop: do not read, copy or analyse the ROM. Ask once per new ROM; a game that already has `games/<name>` was confirmed before.

## 2. Work the phase

Follow the phase in `docs/porting-guide.md`. The files of a game start from `common/template` (`newgame.py` copies it). If another game exists under `games/` (commercial ones are separate repositories, ignored by this one), use it as the worked example for every file type:
- `disasm/layout.toml`
- `ram.md`, `notes/*.md`, `fixes.md`
- `verify/*.txt`
- `port/*.c`, `port/port.h`, `port/screen.c`, `port/main.c`
- `src/game.c`

A commercial game gets its own git repository (`newgame.py` runs `git init` in `games/<name>`): commit the game there, and shared code (`common/`, `docs/`) in this repository. A freely licensed game can stay in this repository (`newgame.py --here`, plus a `!games/<name>/` line in `.gitignore`).

Rules that hold in every phase:

- Build and verify in a login shell: `bash -lc 'make'`, `bash -lc 'make verify'`.
- From phase 4 on, `make verify` must pass after every change to `port/` and `common/`. The port matches the original tick by tick, and only the NDS side may differ.
- Never commit ROMs or anything generated from them (listings, dumped text, extracted data). `build/` is ignored.
- Nothing from a game goes into this repository: no addresses, names or text of a commercial game in `common/` or `docs/`.
- Commit at the end of each step, when the user has asked for commits. Follow the existing history: a gitmoji subject in English, no body. Code and comments are in English.
- Update the README checklist and "Next steps" in the same commit as the work, so the next session can pick it up.
- Things only the user can judge (melonDS checks, listening, translation choices) go to `games/<name>/REVIEW.md`, which is not committed (the game's `.gitignore`). Keep working instead of waiting for them.
- Odd original behaviour goes to `fixes.md` and is not fixed until phase 10.

## 3. Large phases

Phase 5 (porting the code) is most of the work. Split it by PRG page or address range into port files:
1. Port the shared helpers first, yourself.
2. Then give independent ranges to agents running in separate worktrees (`isolation: "worktree"`). Each agent owns its files and writes verify scripts for the code it ports.
3. Merge them one at a time, run `make verify` after each merge, and resolve duplicate helpers.

The coverage list printed by `make verify` shows what is still unchecked.

## 4. Checking the NDS build

From phase 7 on, use the `nds-check` skill (headless DeSmuME: screenshots, shake detection, CPU load). Then ask the user to confirm on melonDS.
