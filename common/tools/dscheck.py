"""Run an NDS build headless in DeSmuME and check it without a person.

Needs py-desmume (see docs/setup.md, "Headless NDS checks"); run it with that
environment's Python. DeSmuME does not model the ARM9 caches, so the CPU load
it reports is an upper bound: melonDS or hardware is lighter.

    dscheck.py build/game.nds --elf build/game.elf --script run.txt \\
        --frames 2400 --shots out/ --every 30 --shake --load

Script lines (NDS frames, the boot logo included; '#' starts a comment):
    700 START            press keys from this frame on (A B X Y L R START
                         SELECT UP DOWN LEFT RIGHT, combined with +)
    706 -                release all keys
    900 touch 128 96     touch the bottom screen at (x, y) for 3 frames
    1800 poke 7C 0F      write NES RAM $007C (needs --elf: symbol `nes`)

Checks:
    --shots DIR   save both screens as DIR/fNNNNN.png (every --every frames,
                  and at every --at frame)
    --shake       report frames where a screen moves vertically as a whole
                  against the previous frame (lost vblanks, broken scroll)
    --load        print the CPU load the platform measured (nesplatform.c
                  `load_shown`, busiest frame of the last second) every 60
                  frames; needs --elf
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys

os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
os.environ.setdefault("SDL_AUDIODRIVER", "dummy")

try:
    from desmume.controls import Keys, keymask
    from desmume.emulator import DeSmuME
except ImportError:
    sys.exit("dscheck.py needs py-desmume, see docs/setup.md (Headless NDS checks)")

KEYS = {
    "A": Keys.KEY_A, "B": Keys.KEY_B, "X": Keys.KEY_X, "Y": Keys.KEY_Y,
    "L": Keys.KEY_L, "R": Keys.KEY_R, "START": Keys.KEY_START, "SELECT": Keys.KEY_SELECT,
    "UP": Keys.KEY_UP, "DOWN": Keys.KEY_DOWN, "LEFT": Keys.KEY_LEFT, "RIGHT": Keys.KEY_RIGHT,
}
NES_RAM_OFFSET = 16  # offsetof(Vnes, ram) on ARM: two pointers, two ints
FRAME_TICKS = 2188   # nesplatform.c: timer ticks in one NDS frame


def symbols(elf: str) -> dict[str, int]:
    nm = os.path.join(os.environ.get("WONDERFUL_TOOLCHAIN", "/opt/wonderful"),
                      "toolchain/gcc-arm-none-eabi/bin/arm-none-eabi-nm")
    out = subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return syms


def load_script(path: str | None) -> dict[int, list[list[str]]]:
    events: dict[int, list[list[str]]] = {}
    if not path:
        return events
    with open(path) as f:
        for line in f:
            words = line.split("#", 1)[0].split()
            if words:
                events.setdefault(int(words[0]), []).append(words[1:])
    return events


def rows(img, top: int) -> list[bytes]:
    return [img.crop((0, top + y, 256, top + y + 1)).tobytes() for y in range(192)]


def shift_of(prev: list[bytes], cur: list[bytes]) -> int:
    """Vertical shift that explains most of the change, 0 if none does."""
    blank = bytes(256 * 3)
    if sum(1 for r in cur if r != blank) < 40:
        return 0  # (nearly) empty screen: nothing to compare
    same = sum(1 for a, b in zip(prev, cur) if a != b)
    best, best_diff = 0, same
    for s in range(-24, 25):
        if s:
            d = sum(1 for y in range(30, 160) if prev[y] != cur[y + s])
            if d < best_diff:
                best, best_diff = s, d
    return best if best and best_diff < same * 0.5 else 0


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("nds")
    ap.add_argument("--elf", help="ELF of the build, for poke and --load")
    ap.add_argument("--script", help="input script")
    ap.add_argument("--frames", type=int, default=1200)
    ap.add_argument("--shots", help="directory for screenshots")
    ap.add_argument("--every", type=int, default=0, help="screenshot every N frames")
    ap.add_argument("--at", default="", help="comma separated frames to screenshot")
    ap.add_argument("--shake", action="store_true")
    ap.add_argument("--load", action="store_true")
    args = ap.parse_args()

    syms = symbols(args.elf) if args.elf else {}
    events = load_script(args.script)
    at = {int(x) for x in args.at.split(",") if x}
    if args.shots:
        os.makedirs(args.shots, exist_ok=True)

    emu = DeSmuME()
    emu.open(args.nds)
    emu.volume_set(0)
    held: set[int] = set()
    touch_until = -1
    prev = None
    shakes = []

    for f in range(args.frames):
        for ev in events.get(f, []):
            if ev[0] == "-":
                for k in held:
                    emu.input.keypad_rm_key(keymask(k))
                held.clear()
            elif ev[0] == "touch":
                emu.input.touch_set_pos(int(ev[1]), int(ev[2]))
                touch_until = f + 3
            elif ev[0] == "poke":
                if "nes" not in syms:
                    sys.exit("poke needs --elf with the `nes` symbol (nesplatform.c)")
                emu.memory.write_byte(syms["nes"] + NES_RAM_OFFSET + int(ev[1], 16), int(ev[2], 16))
            else:
                for name in ev[0].split("+"):
                    k = KEYS[name.upper()]
                    emu.input.keypad_add_key(keymask(k))
                    held.add(k)
        if f == touch_until:
            emu.input.touch_release()
        emu.cycle(with_joystick=False)

        need_img = args.shake or (args.shots and ((args.every and f % args.every == 0) or f in at))
        if need_img:
            img = emu.screenshot().convert("RGB")
            if args.shots and ((args.every and f % args.every == 0) or f in at):
                img.save(os.path.join(args.shots, f"f{f:05d}.png"))
            if args.shake:
                cur = (rows(img, 0), rows(img, 192))
                if prev:
                    for scr, name in ((0, "top"), (1, "bottom")):
                        if cur[scr] != prev[scr]:
                            s = shift_of(prev[scr], cur[scr])
                            if s:
                                shakes.append((f, name, s))
                prev = cur
        if args.load and f % 60 == 59:
            if "load_shown" not in syms:
                sys.exit("--load needs --elf with the `load_shown` symbol (nesplatform.c)")
            ticks = emu.memory.unsigned.read_short(syms["load_shown"])
            print(f"frame {f + 1:5d}  load {ticks * 100 // FRAME_TICKS:3d}%")

    if args.shake:
        print(f"shake: {len(shakes)} frame(s) moved as a whole")
        for f, name, s in shakes:
            print(f"  frame {f} {name} screen moved {s:+d} lines")


if __name__ == "__main__":
    main()
