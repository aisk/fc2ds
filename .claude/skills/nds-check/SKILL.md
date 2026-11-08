---
name: nds-check
description: Run an NDS build of this repository headless in DeSmuME to check it without the user. Covers screenshots, scripted input and RAM pokes, frames where the picture jumps, and CPU load. Use after changing the renderer, the platform layer, sound or timing, or when the user reports a visual glitch or slowdown.
---

# Headless NDS check

Tool: `common/tools/dscheck.py`. Its docstring has the script format. It needs py-desmume:

```sh
test -x ~/.local/share/fc2ds/dsenv/bin/python || {
  uv venv --python 3.12 ~/.local/share/fc2ds/dsenv &&
  uv pip install --python ~/.local/share/fc2ds/dsenv/bin/python py-desmume pillow; }
DS=~/.local/share/fc2ds/dsenv/bin/python
```

No root is needed. If `uv` is missing, ask the user to install it.

## Typical runs

From `games/<name>`, after `bash -lc make`:

```sh
$DS ../../common/tools/dscheck.py build/<name>.nds --elf build/<name>.elf \
    --script run.txt --frames 2400 --shots /tmp/out --every 60 --shake --load
```

- Write the input script in the scratchpad. Frames count from power on: the game starts some 70 frames in, and title screens take a while.
  - `N START` presses a key.
  - `N -` releases all keys.
  - `N touch X Y` touches the bottom screen.
  - `N poke ADDR VAL` writes NES RAM. Use it to jump to a stage, the same pokes the game's `verify/*.txt` scripts use.
- Look at the screenshots: Read the PNGs, and paste several into one contact sheet with Pillow to save reads.
- `--shake` lists frames where a whole screen moved vertically against the previous one: lost vblanks or broken scroll. A fade from black can be a false positive, so check those frames in the screenshots.
- `--load` prints the busiest frame of each second, as a percentage of a frame. 100% or more means dropped frames.

## Reading the numbers

- DeSmuME does not model the ARM9 caches, so code and data in main RAM look slower than on melonDS or hardware. Its load is an upper bound.
- Under 70% in DeSmuME has been fine on melonDS.
- To find where the time goes, temporarily read `TIMER_DATA(2)` at points in `present()` (`common/nds/src/nesplatform.c`) into a global array, then read that array with `emu.memory.unsigned.read_short`, using the symbol's address from `arm-none-eabi-nm`. Remove the instrumentation before committing.
- Compare with the host first when the question is "is the picture right". `nesref --shot` writes the NES frame and the NDS screens for the same tick, and those must agree. DeSmuME is for timing and hardware behaviour (DMA, windows, VRAM).

The final word is always the user on melonDS. Add what they should look at to `games/<name>/REVIEW.md`.
