"""Recursive-descent 6502 code tracer that writes da65 info files.

The PRG ROM is described as one fixed segment plus switchable pages that share
one CPU window (e.g. MMC3 games that always switch 16 KiB at $8000-$BFFF).
Starting from the vectors and extra entry points in the config, the tracer
follows control flow, tracks which page is mapped (a constant loaded into A
right before a call to the bank switch routine), and decodes inline jump tables
used by "jsr Dispatch / .word ..." routines.

Everything it can prove is code becomes a CODE range, jump tables become
ADDRTABLE, the rest is left as BYTETABLE. Hand-written names and comments from
the config are merged in, so the generated info files never need hand edits.

Config: TOML, see games/<name>/disasm/layout.toml.
"""
from __future__ import annotations

import argparse
import tomllib
from collections import defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path

import nesrom

# --------------------------------------------------------------------------
# Opcode table (official opcodes only)

MODE_LEN = {"imp": 1, "acc": 1, "imm": 2, "zp": 2, "zpx": 2, "zpy": 2, "izx": 2,
            "izy": 2, "rel": 2, "abs": 3, "abx": 3, "aby": 3, "ind": 3}

OPS: dict[int, tuple[str, str]] = {}


def _group(mnem: str, codes: str, modes: str) -> None:
    for c, m in zip(codes.split(), modes.split()):
        OPS[int(c, 16)] = (mnem, m)


_STD = "imm zp zpx abs abx aby izx izy"
for _m, _c in (("ora", "09 05 15 0D 1D 19 01 11"), ("and", "29 25 35 2D 3D 39 21 31"),
               ("eor", "49 45 55 4D 5D 59 41 51"), ("adc", "69 65 75 6D 7D 79 61 71"),
               ("lda", "A9 A5 B5 AD BD B9 A1 B1"), ("cmp", "C9 C5 D5 CD DD D9 C1 D1"),
               ("sbc", "E9 E5 F5 ED FD F9 E1 F1")):
    _group(_m, _c, _STD)
_group("sta", "85 95 8D 9D 99 81 91", "zp zpx abs abx aby izx izy")
for _m, _c in (("asl", "0A 06 16 0E 1E"), ("rol", "2A 26 36 2E 3E"),
               ("lsr", "4A 46 56 4E 5E"), ("ror", "6A 66 76 6E 7E")):
    _group(_m, _c, "acc zp zpx abs abx")
_group("dec", "C6 D6 CE DE", "zp zpx abs abx")
_group("inc", "E6 F6 EE FE", "zp zpx abs abx")
_group("ldx", "A2 A6 B6 AE BE", "imm zp zpy abs aby")
_group("ldy", "A0 A4 B4 AC BC", "imm zp zpx abs abx")
_group("stx", "86 96 8E", "zp zpy abs")
_group("sty", "84 94 8C", "zp zpx abs")
_group("cpx", "E0 E4 EC", "imm zp abs")
_group("cpy", "C0 C4 CC", "imm zp abs")
_group("bit", "24 2C", "zp abs")
for _m, _c in (("bpl", "10"), ("bmi", "30"), ("bvc", "50"), ("bvs", "70"),
               ("bcc", "90"), ("bcs", "B0"), ("bne", "D0"), ("beq", "F0")):
    _group(_m, _c, "rel")
_group("jmp", "4C 6C", "abs ind")
_group("jsr", "20", "abs")
for _m, _c in (("brk", "00"), ("php", "08"), ("clc", "18"), ("plp", "28"), ("sec", "38"),
               ("rti", "40"), ("pha", "48"), ("cli", "58"), ("rts", "60"), ("pla", "68"),
               ("sei", "78"), ("dey", "88"), ("txa", "8A"), ("tya", "98"), ("txs", "9A"),
               ("tay", "A8"), ("tax", "AA"), ("clv", "B8"), ("tsx", "BA"), ("iny", "C8"),
               ("dex", "CA"), ("cld", "D8"), ("inx", "E8"), ("nop", "EA"), ("sed", "F8")):
    _group(_m, _c, "imp")

# Instructions that change A (so a tracked constant in A is lost).
A_WRITERS = {"lda", "adc", "sbc", "and", "ora", "eor", "pla", "txa", "tya"}

HW_REGS = {
    0x2000: "PPUCTRL", 0x2001: "PPUMASK", 0x2002: "PPUSTATUS", 0x2003: "OAMADDR",
    0x2004: "OAMDATA", 0x2005: "PPUSCROLL", 0x2006: "PPUADDR", 0x2007: "PPUDATA",
    0x4000: "SQ1_VOL", 0x4001: "SQ1_SWEEP", 0x4002: "SQ1_LO", 0x4003: "SQ1_HI",
    0x4004: "SQ2_VOL", 0x4005: "SQ2_SWEEP", 0x4006: "SQ2_LO", 0x4007: "SQ2_HI",
    0x4008: "TRI_LINEAR", 0x400A: "TRI_LO", 0x400B: "TRI_HI",
    0x400C: "NOISE_VOL", 0x400E: "NOISE_LO", 0x400F: "NOISE_HI",
    0x4010: "DMC_FREQ", 0x4011: "DMC_RAW", 0x4012: "DMC_START", 0x4013: "DMC_LEN",
    0x4014: "OAMDMA", 0x4015: "SND_CHN", 0x4016: "JOY1", 0x4017: "JOY2",
}

# --------------------------------------------------------------------------
# Memory model


@dataclass
class Segment:
    name: str
    prg: int
    base: int
    size: int
    page: int | None  # page number for switchable segments, None for fixed
    data: bytes = b""
    # per-byte type: 0 = unknown/data, 1 = code, 2 = address table
    kind: bytearray = field(default_factory=bytearray)
    entries: dict[int, str] = field(default_factory=dict)  # addr -> label prefix

    def has(self, addr: int) -> bool:
        return self.base <= addr < self.base + self.size

    def byte(self, addr: int) -> int:
        return self.data[addr - self.base]


@dataclass
class Layout:
    fixed: Segment
    pages: list[Segment]
    window: tuple[int, int]
    bank_switch: int
    dispatchers: set[int]

    def resolve(self, addr: int, cur: Segment, ctx: int | None) -> Segment | None:
        """Which segment an absolute address lands in, given the current page."""
        if self.fixed.has(addr):
            return self.fixed
        if self.window[0] <= addr < self.window[1]:
            if cur.page is not None:
                return cur
            if ctx is not None and 0 <= ctx < len(self.pages):
                return self.pages[ctx]
        return None

    def segments(self) -> list[Segment]:
        return self.pages + [self.fixed]


def load_layout(cfg: dict, rom: nesrom.NesRom) -> Layout:
    lay = cfg["layout"]
    psize = lay["page_size"]
    win = lay["window"]
    pages = []
    for p in range(lay["pages"]):
        pages.append(Segment(f"page{p}", p * psize, win, psize, p))
    fixed = Segment("fixed", lay["fixed_prg"], lay["fixed_base"], lay["fixed_size"], None)
    for s in pages + [fixed]:
        s.data = rom.prg[s.prg:s.prg + s.size]
        s.kind = bytearray(s.size)
    return Layout(fixed, pages, (win, win + psize), lay["bank_switch"], set(lay.get("dispatchers", [])))


def plausible_code(seg: Segment, addr: int, steps: int = 8) -> bool:
    """Cheap check that `addr` starts real code: no illegal opcode or brk soon."""
    for _ in range(steps):
        if not seg.has(addr) or seg.kind[addr - seg.base] == 2:
            return False
        op = seg.byte(addr)
        if op not in OPS or op == 0x00:
            return False
        mnem, mode = OPS[op]
        # Stop at anything that may not fall through (a jsr may be followed by
        # an inline table).
        if mnem in ("rts", "rti", "jmp", "jsr"):
            return True
        addr += MODE_LEN[mode]
    return True


# --------------------------------------------------------------------------
# Tracer


@dataclass
class Tracer:
    lay: Layout
    table_len: dict[tuple[str, int], int]
    code_ptrs: dict[int, int] = field(default_factory=dict)  # hi var -> lo var
    table_ptrs: dict[int, int] = field(default_factory=dict)  # hi var -> lo var
    pending: set = field(default_factory=set)  # (seg name, addr, ctx) tables found while walking
    seen: set = field(default_factory=set)
    origin: dict[tuple[str, int], str] = field(default_factory=dict)  # entry -> who added it
    entry_of: dict[tuple[str, int], int] = field(default_factory=dict)  # instruction -> walk entry
    work: deque = field(default_factory=deque)
    tables: dict[str, list[tuple[int, int]]] = field(default_factory=lambda: defaultdict(list))
    unresolved: list[str] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)
    indirect: list[str] = field(default_factory=list)
    insns: dict = field(default_factory=dict)  # (seg, addr) -> (mnem, mode, operand)
    auto_tables: set = field(default_factory=set)  # (seg, start) whose length was guessed
    truncate: dict = field(default_factory=dict)  # (seg, start) -> entries, for the next pass

    def push(self, seg: Segment, addr: int, ctx: int | None, prefix: str = "L", src: str = "") -> None:
        self.origin.setdefault((seg.name, addr), src)
        if seg.page is not None:
            ctx = seg.page
        old = seg.entries.get(addr)
        if old is None or (old == "L" and prefix != "L"):
            seg.entries[addr] = prefix
        self.work.append((seg, addr, ctx, None))

    def target(self, addr: int, seg: Segment, ctx: int | None, src: int, prefix: str) -> Segment | None:
        t = self.lay.resolve(addr, seg, ctx)
        if t is None:
            if addr >= 0x8000:
                self.unresolved.append(f"{seg.name}:{src:04X} -> ${addr:04X} (page unknown)")
            else:
                self.unresolved.append(f"{seg.name}:{src:04X} -> ${addr:04X} (RAM)")
            return None
        self.push(t, addr, ctx, prefix, f"{seg.name}:{src:04X}")
        return t

    def word_table(self, seg: Segment, start: int, ctx: int | None, src: int, count: int | None,
                   end: int | None = None) -> None:
        """Decode a table of code pointers starting at `start` in `seg`.

        Without `count`, the table ends at `end`, at the first entry that is
        not a valid code address, or where the first target in this segment
        begins (tables usually sit right before the code they point to).
        """
        if count is None:
            count = self.table_len.get((seg.name, start))
        if count is None:
            self.auto_tables.add((seg.name, start))
        limit = seg.base + seg.size if end is None else min(end, seg.base + seg.size)
        pos = start
        first_target = limit
        n = 0
        while pos + 1 < limit:
            if count is not None and n >= count:
                break
            if count is None and (pos >= first_target or n >= 128):
                break
            if count is None and pos != start and (pos in seg.entries or seg.kind[pos - seg.base] == 1):
                break
            w = seg.byte(pos) | (seg.byte(pos + 1) << 8)
            t = self.lay.resolve(w, seg, ctx)
            if t is None or not t.has(w) or (count is None and not plausible_code(t, w)):
                if count is None:
                    break
                self.unresolved.append(f"{seg.name}:{pos:04X} table entry -> ${w:04X}")
            else:
                if t is seg and w > start:
                    first_target = min(first_target, w)
                self.push(t, w, ctx, "J", f"{seg.name}:{pos:04X} (table)")
            seg.kind[pos - seg.base] = 2
            seg.kind[pos + 1 - seg.base] = 2
            pos += 2
            n += 1
        self.tables[seg.name].append((start, pos - 1))

    def table_of_tables(self, seg: Segment, start: int, ctx: int | None, count: int | None) -> None:
        """An outer table of pointers to inner code pointer tables."""
        inner: set[tuple[str, int]] = set()
        segs = {s.name: s for s in self.lay.segments()}
        if count is None:
            count = self.table_len.get((seg.name, start))
        if count is None:
            self.auto_tables.add((seg.name, start))
        pos = start
        limit = seg.base + seg.size
        first = limit
        n = 0
        while pos + 1 < limit:
            if count is not None and n >= count:
                break
            if count is None and pos >= first:
                break
            w = seg.byte(pos) | (seg.byte(pos + 1) << 8)
            t = self.lay.resolve(w, seg, ctx)
            if t is None:
                if count is None:
                    break
                self.unresolved.append(f"{seg.name}:{pos:04X} inner table -> ${w:04X}")
            else:
                inner.add((t.name, w))
                if t is seg and w > start:
                    first = min(first, w)
            seg.kind[pos - seg.base] = 2
            seg.kind[pos + 1 - seg.base] = 2
            pos += 2
            n += 1
        self.tables[seg.name].append((start, pos - 1))
        # Inner tables are usually packed back to back, so each one ends where
        # the next one (in the same segment) begins.
        for tname, w in sorted(inner):
            t = segs[tname]
            later = [b for s, b in inner if s == tname and b > w]
            if t is seg and start > w:
                later.append(start)
            t.entries.setdefault(w, "T")
            self.word_table(t, w, ctx, start, None, min(later) if later else None)

    def run(self) -> None:
        segs = {s.name: s for s in self.lay.segments()}
        done: set = set()
        while True:
            while self.work:
                seg, addr, ctx, a = self.work.popleft()
                self.walk(seg, addr, ctx, a)
            todo = sorted(self.pending - done)
            if not todo:
                return
            done |= self.pending
            starts = {(n, a) for n, a, _ in self.pending}
            for sname, start, ctx in todo:
                t = segs[sname]
                later = [b for n, b in starts if n == sname and b > start]
                t.entries.setdefault(start, "T")
                self.word_table(t, start, ctx, start, None, min(later) if later else None)

    def chain(self, seg: str, addr: int) -> str:
        """Describe how an entry point was reached, following the origin chain."""
        parts = []
        key = (seg, addr)
        for _ in range(6):
            src = self.origin.get(key)
            if not src:
                break
            parts.append(src)
            sname, rest = src.split(":", 1)
            key = (sname, int(rest[:4], 16))
            # Walk back to the entry of the routine containing `src`.
            key = (sname, self.entry_of.get(key, key[1]))
        return " <- ".join(parts)

    def err(self, seg: Segment, entry: int, msg: str) -> None:
        self.errors.append(f"{msg}  [entry {seg.name}:{entry:04X} <- {self.chain(seg.name, entry)}]")

    def walk(self, seg: Segment, addr: int, ctx: int | None, a: int | None) -> None:
        lay = self.lay
        imm_store: dict[int, int] = {}
        entry = addr
        while True:
            key = (seg.name, addr, ctx)
            if key in self.seen:
                return
            if not seg.has(addr):
                self.err(seg, entry, f"{seg.name}: execution runs off segment at ${addr:04X}")
                return
            off = addr - seg.base
            if seg.kind[off] == 2:
                for start, end in self.tables[seg.name]:
                    if start <= addr <= end and (seg.name, start) in self.auto_tables:
                        # A guessed table ate real code: shorten it next pass.
                        n = (addr - start) // 2
                        key2 = (seg.name, start)
                        self.truncate[key2] = min(n, self.truncate.get(key2, n))
                        return
                self.err(seg, entry, f"{seg.name}:{addr:04X} code flows into an address table")
                return
            self.seen.add(key)
            self.entry_of.setdefault((seg.name, addr), entry)
            op = seg.byte(addr)
            if op not in OPS:
                self.err(seg, entry, f"{seg.name}:{addr:04X} illegal opcode ${op:02X}")
                return
            mnem, mode = OPS[op]
            n = MODE_LEN[mode]
            if off + n > seg.size:
                self.err(seg, entry, f"{seg.name}:{addr:04X} instruction crosses segment end")
                return
            for i in range(n):
                seg.kind[off + i] = 1
            operand = 0
            if n == 2:
                operand = seg.byte(addr + 1)
            elif n == 3:
                operand = seg.byte(addr + 1) | (seg.byte(addr + 2) << 8)
            nxt = addr + n
            self.insns[(seg.name, addr)] = (mnem, mode, operand)

            if mnem == "brk":
                self.err(seg, entry, f"{seg.name}:{addr:04X} brk reached (probably data)")
                return
            if mnem in ("rts", "rti"):
                return
            if mode == "rel":
                dest = (nxt + (operand - 256 if operand & 0x80 else operand)) & 0xFFFF
                if seg.has(dest):
                    self.origin.setdefault((seg.name, dest), f"{seg.name}:{addr:04X}")
                    self.work.append((seg, dest, ctx, a))
                    seg.entries.setdefault(dest, "L")
                else:
                    self.err(seg, entry, f"{seg.name}:{addr:04X} branch leaves segment")
            elif mnem == "jmp" and mode == "ind":
                self.indirect.append(f"{seg.name}:{addr:04X} jmp (${operand:04X})")
                return
            elif mnem == "jmp":
                if operand == lay.bank_switch:
                    return
                self.target(operand, seg, ctx, addr, "L")
                return
            elif mnem == "jsr":
                if operand in lay.dispatchers:
                    count = self.table_len.get((seg.name, nxt))
                    self.word_table(seg, nxt, ctx, addr, count)
                    return
                if operand == lay.bank_switch:
                    if a is not None and seg.page is None:
                        ctx = a
                    elif seg.page is None:
                        ctx = None
                else:
                    self.target(operand, seg, ctx, addr, "S")
                a = None
            elif mnem == "lda" and mode == "imm":
                a = operand
            elif mnem == "sta" and a is not None and mode in ("zp", "zpx", "abs", "abx", "aby"):
                imm_store[operand] = a
                lo_var = self.code_ptrs.get(operand)
                if lo_var is not None and lo_var in imm_store:
                    dest = imm_store[lo_var] | (a << 8)
                    if dest >= 0x8000:
                        self.target(dest, seg, ctx, addr, "P")
                lo_var = self.table_ptrs.get(operand)
                if lo_var is not None and lo_var in imm_store:
                    dest = imm_store[lo_var] | (a << 8)
                    t = lay.resolve(dest, seg, ctx)
                    if t is not None:
                        self.origin.setdefault((t.name, dest), f"{seg.name}:{addr:04X}")
                        self.pending.add((t.name, dest, ctx))
            elif mnem in A_WRITERS or (mode == "acc"):
                a = None
            addr = nxt


# --------------------------------------------------------------------------
# Output


def ranges(kind: bytearray, base: int):
    start = 0
    for i in range(1, len(kind) + 1):
        if i == len(kind) or kind[i] != kind[start]:
            yield base + start, base + i - 1, kind[start]
            start = i


def parse_names(cfg: dict) -> dict[str, dict[int, tuple[str, str]]]:
    out: dict[str, dict[int, tuple[str, str]]] = defaultdict(dict)
    for seg, items in cfg.get("labels", {}).items():
        for addr, v in items.items():
            if isinstance(v, str):
                v = {"name": v}
            out[seg][int(addr, 16)] = (v.get("name", ""), v.get("comment", ""))
    return out


def label_line(name: str, addr: int, comment: str = "", size: int = 0) -> str:
    s = f'LABEL {{ NAME "{name}"; ADDR ${addr:04X};'
    if size > 1:
        s += f" SIZE {size};"
    if comment:
        s += ' COMMENT "' + comment.replace("\\", "/").replace('"', "'") + '";'
    return s + " };"


def write_info(seg: Segment, lay: Layout, names, ram: dict[int, tuple[str, str, int]],
               tables, outdir: Path) -> None:
    lines = [
        "# Generated by common/tools/trace6502.py, do not edit.",
        "# Edit the layout/labels config and regenerate instead.",
        "GLOBAL {",
        f'    INPUTNAME "{seg.name}.bin";',
        f'    OUTPUTNAME "{seg.name}.s";',
        f"    STARTADDR ${seg.base:04X};",
        '    CPU "6502";',
        "    COMMENTS 4;",
        "};",
        "",
    ]
    table_ranges = {(s, e) for s, e in tables.get(seg.name, [])}
    for s, e, k in ranges(seg.kind, seg.base):
        if k == 1:
            lines.append(f"RANGE {{ START ${s:04X}; END ${e:04X}; TYPE CODE; }};")
        elif k == 2:
            lines.append(f"RANGE {{ START ${s:04X}; END ${e:04X}; TYPE ADDRTABLE; }};")
        else:
            lines.append(f"RANGE {{ START ${s:04X}; END ${e:04X}; TYPE BYTETABLE; }};")
    del table_ranges
    lines.append("")

    used: set[str] = set()

    def add(name: str, addr: int, comment: str = "", size: int = 0) -> None:
        if name in used:
            name = f"{name}_{addr:04X}"
        used.add(name)
        lines.append(label_line(name, addr, comment, size))

    own = names.get(seg.name, {})
    for addr in sorted(set(seg.entries) | set(own)):
        name, comment = own.get(addr, ("", ""))
        if not name:
            name = f"{seg.entries[addr]}{addr:04X}"
        add(name, addr, comment)

    # Names from other segments that are always visible (fixed), registers and RAM.
    if seg is not lay.fixed:
        fixed_names = names.get("fixed", {})
        for addr in sorted(set(lay.fixed.entries) | set(fixed_names)):
            name, _ = fixed_names.get(addr, ("", ""))
            add(name or f"{lay.fixed.entries[addr]}{addr:04X}", addr)
    for addr, reg in HW_REGS.items():
        add(reg, addr)
    ram_items = sorted(ram.items())
    for i, (addr, (name, comment, size)) in enumerate(ram_items):
        # da65 rejects a label inside another label's range: clip the range.
        if i + 1 < len(ram_items):
            size = min(size, ram_items[i + 1][0] - addr)
        add(name, addr, comment, size)

    (outdir / f"{seg.name}.info").write_text("\n".join(lines) + "\n")


RMW = {"inc", "dec", "asl", "lsr", "rol", "ror"}
WRITES = {"sta", "stx", "sty"}


def ram_xref(lay: Layout, tr: Tracer, ram: dict) -> list[str]:
    """Who reads/writes each RAM address ($0000-$07FF), grouped by routine."""
    routine_starts = {}
    for s in lay.segments():
        routine_starts[s.name] = sorted(a for a, p in s.entries.items() if p != "L")
    import bisect

    def routine(sname: str, addr: int) -> str:
        starts = routine_starts[sname]
        i = bisect.bisect_right(starts, addr) - 1
        return f"{sname}:{starts[i]:04X}" if i >= 0 else f"{sname}:?"

    use: dict[int, dict[str, set[str]]] = defaultdict(lambda: defaultdict(set))
    for (sname, addr), (mnem, mode, operand) in tr.insns.items():
        if mode in ("imp", "acc", "imm", "rel") or mnem in ("jmp", "jsr"):
            continue
        if operand >= 0x800:
            continue
        r = routine(sname, addr)
        if mode in ("izx", "izy"):
            use[operand]["p"].add(r)
            use[operand + 1]["p"].add(r)
            kind = "w" if mnem in WRITES else "r"
            use[operand]["*" + kind].add(r)
            continue
        idx = mode in ("zpx", "zpy", "abx", "aby")
        if mnem in WRITES:
            k = "w"
        elif mnem in RMW:
            k = "m"
        else:
            k = "r"
        use[operand][k + ("x" if idx else "")].add(r)

    out = ["# RAM cross reference (generated)",
           "# r/w/m = read/write/read-modify-write, x = indexed, p = used as pointer, *r/*w = through pointer",
           ""]
    for addr in sorted(use):
        name = ram.get(addr, ("", "", 1))[0]
        parts = []
        for k in ("w", "wx", "m", "mx", "r", "rx", "p", "*r", "*w"):
            if use[addr].get(k):
                rs = sorted(use[addr][k])
                more = f" +{len(rs) - 6}" if len(rs) > 6 else ""
                parts.append(f"{k}:{len(rs)} [{' '.join(rs[:6])}{more}]")
        out.append(f"${addr:04X} {name:16s} " + "  ".join(parts))
    return out


def names_from_markdown(path: Path) -> tuple[dict, dict]:
    """Read RAM and routine names from Markdown tables.

    A row whose first cell is `$XX` or `$XXXX-$YYYY` names RAM (second cell is
    the name, third the meaning). A row whose first cell is `seg:XXXX` names a
    routine (second cell name, third a comment).
    """
    import re
    ram: dict[int, tuple[str, str, int]] = {}
    labels: dict[str, dict[int, tuple[str, str]]] = defaultdict(dict)
    conflicts: list[str] = []
    for line in path.read_text().splitlines():
        if not line.lstrip().startswith("|"):
            continue
        row = line.strip()[1:].rstrip("|")
        cells = [c.strip().replace("`", "").replace("\\|", "|") for c in re.split(r"(?<!\\)\|", row)]
        if len(cells) < 2:
            continue
        # Name cell: one identifier, optionally followed by a "(guess)" style note.
        m = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_]*)(\s*\(.*\))?", cells[1])
        if not m:
            continue
        name = m.group(1)
        comment = cells[2] if len(cells) > 2 else ""
        if m.group(2):
            comment = f"{m.group(2).strip()} {comment}"
        first = re.sub(r",\s*[xy]$", "", cells[0])
        m = re.fullmatch(r"\$([0-9A-Fa-f]{2,4})(?:\s*-\s*\$?([0-9A-Fa-f]{2,4}))?", first)
        if m:
            a = int(m.group(1), 16)
            b = int(m.group(2), 16) if m.group(2) else a
            if a in ram and ram[a][0] != name:
                conflicts.append(f"{path.name}: ${a:04X} {name} vs {ram[a][0]}")
                continue
            ram[a] = (name, comment, b - a + 1)
            continue
        m = re.fullmatch(r"(\w+):\$?([0-9A-Fa-f]{4})", first)
        if m:
            labels[m.group(1)][int(m.group(2), 16)] = (name, comment)
    for c in conflicts:
        print(f"name conflict (first one wins): {c}")
    return ram, labels


def parse_ram(cfg: dict) -> dict[int, tuple[str, str, int]]:
    out = {}
    for addr, v in cfg.get("ram", {}).items():
        if isinstance(v, str):
            v = {"name": v}
        out[int(addr, 16)] = (v["name"], v.get("comment", ""), v.get("size", 1))
    return out


def trace(cfg: dict, rom: nesrom.NesRom, table_len: dict) -> tuple[Layout, Tracer]:
    lay = load_layout(cfg, rom)
    segs = {s.name: s for s in lay.segments()}
    tr = Tracer(lay, dict(table_len))
    for lo, hi in cfg.get("code_pointers", []):
        tr.code_ptrs[hi] = lo
    for lo, hi in cfg.get("table_pointers", []):
        tr.table_ptrs[hi] = lo

    # Vectors
    vec = lay.fixed.base + lay.fixed.size - 6
    for i in range(3):
        p = vec + i * 2
        w = lay.fixed.byte(p) | (lay.fixed.byte(p + 1) << 8)
        lay.fixed.kind[p - lay.fixed.base] = 2
        lay.fixed.kind[p + 1 - lay.fixed.base] = 2
        tr.push(lay.fixed, w, None, "S")
    tr.tables["fixed"].append((vec, vec + 5))

    for e in cfg.get("entry", []):
        tr.push(segs[e["seg"]], e["addr"], e.get("ctx"), "S")
    tr.run()
    for t in cfg.get("table", []):
        seg = segs[t["seg"]]
        if t.get("depth", 1) == 2:
            tr.table_of_tables(seg, t["addr"], t.get("ctx"), t.get("count"))
        else:
            tr.word_table(seg, t["addr"], t.get("ctx"), t["addr"], t.get("count"))
        tr.run()
    return lay, tr


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("config")
    ap.add_argument("rom")
    ap.add_argument("outdir")
    args = ap.parse_args()

    cfg = tomllib.loads(Path(args.config).read_text())
    rom = nesrom.load(args.rom)
    if "sha1" in cfg.get("rom", {}) and rom.sha1 != cfg["rom"]["sha1"]:
        raise SystemExit(f"{args.rom}: SHA-1 does not match config")
    table_len = {}
    for key, n in cfg.get("table_len", {}).items():
        sname, addr = key.split(":")
        table_len[(sname, int(addr, 16))] = n

    # Guessed table lengths can swallow code that is only discovered later, so
    # rerun from scratch with corrected lengths until nothing changes.
    shortened = {}
    for _ in range(16):
        lay, tr = trace(cfg, rom, table_len)
        if not tr.truncate:
            break
        table_len.update(tr.truncate)
        shortened.update(tr.truncate)

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    names = parse_names(cfg)
    ram = parse_ram(cfg)
    for md in cfg.get("names_from", []):
        md_ram, md_labels = names_from_markdown(Path(args.config).parent / md)
        for k, v in md_ram.items():
            if k in ram and ram[k][0] != v[0]:
                print(f"name conflict (first one wins): {md}: ${k:04X} {v[0]} vs {ram[k][0]}")
            ram.setdefault(k, v)
        for seg, items in md_labels.items():
            for k, v in items.items():
                names[seg].setdefault(k, v)
    for s in lay.segments():
        (outdir / f"{s.name}.bin").write_bytes(s.data)
        write_info(s, lay, names, ram, tr.tables, outdir)

    report = ["# Trace report", ""]
    for s in lay.segments():
        code = sum(1 for k in s.kind if k == 1)
        tab = sum(1 for k in s.kind if k == 2)
        report.append(f"{s.name:6s} ${s.base:04X}  code {code * 100 // s.size:3d}%  "
                      f"tables {tab:5d} bytes  entries {len(s.entries)}")
    known = cfg.get("indirect", {})
    indirect = [i for i in tr.indirect if i.split(" ")[0] not in known]
    for title, items in (("Unresolved targets", tr.unresolved), ("Unexplained indirect jumps", indirect),
                         ("Errors", tr.errors)):
        items = sorted(set(items))
        report += ["", f"## {title} ({len(items)})"] + items
    report += ["", f"## Guessed tables shortened because code runs into them ({len(shortened)})"]
    report += [f"{seg}:{addr:04X} -> {n} entries" for (seg, addr), n in sorted(shortened.items())]
    (outdir / "report.txt").write_text("\n".join(report) + "\n")
    (outdir / "ramxref.txt").write_text("\n".join(ram_xref(lay, tr, ram)) + "\n")
    print("\n".join(report[:2 + len(lay.segments())]))
    print(f"unresolved {len(set(tr.unresolved))}, unexplained indirect {len(set(indirect))}, "
          f"errors {len(set(tr.errors))} (see {outdir / 'report.txt'})")


if __name__ == "__main__":
    main()
