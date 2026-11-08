// Headless reference runner for MMC3 NES games.
//
// Runs the original ROM with scripted input and stops at a given PC once per
// main loop pass ("tick"). There is no picture or sound: common/vnes keeps
// the PPU memory, registers, vblank/NMI timing and the scanline clock that
// drives the MMC3 IRQ counter.
//
// With --compare, two machines run in lockstep: "ref" runs the original code,
// "port" runs it with the game's hooks (ported C routines) in place of the
// original routines. After every tick their RAM, PPU memory (nametables,
// palette, OAM) and PPU/MMC3 registers are compared and the first difference
// is reported.
//
// Usage: nesref ROM --tick-pc ADDR [options]
//   --tick-pc ADDR   PC where a tick ends (RAM is compared / dumped there)
//   --ticks N        stop after N ticks (default 600)
//   --input FILE     input script, one "TICK BUTTONS" per line: from that
//                    tick on the pad holds BUTTONS (letters ABsSUDLR or "-");
//                    "TICK end" stops there unless --ticks is given
//   --speed N        run the CPU N times faster than real hardware, so the
//                    original never lags (default 1; use 8 or more with
//                    --compare since hooks take no CPU time)
//   --compare        run ref and port and compare RAM every tick
//   --ignore A-B     RAM range left out of the comparison (repeatable)
//   --out FILE       binary RAM dump: per tick u32 tick, u32 frame, RAM[0x800]
//   --ppu FILE       write the PPU memory at the end (nametables 4K,
//                    palette 32, OAM 256, MMC3 R0-R7)
//   --trace          print every instruction (slow)
//
// The pad is applied per tick, not per frame, so lag frames in the original
// do not shift the input.
#include "nesref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vnes.h"

// The game's hook table; absent when nesref is built on its own.
static const PortHook *hooks;
static int hook_count;

typedef struct {
    const char *name;
    Vnes nes;
    Cpu cpu;
    long tick;
    int hooks; // run port_hooks
} Machine;

static Machine machines[2];
static Machine *cur;
static long tick_pc = -1;
static int trace;
static long *hook_calls;

static uint8_t bus_read(void *ctx, uint16_t a)
{
    (void)ctx;
    return vnesRead(a);
}

static void bus_write(void *ctx, uint16_t a, uint8_t v)
{
    (void)ctx;
    vnesWrite(a, v);
}

static void use_machine(Machine *m)
{
    cur = m;
    vnes = &m->nes;
}

// Input script: sorted (tick, buttons) pairs.
typedef struct {
    long tick;
    uint8_t pad;
} InputEvent;

static InputEvent *events;
static int event_count;
static long input_end = -1;

static uint8_t parse_buttons(const char *s)
{
    // Bit order as read from $4016: A B Select Start Up Down Left Right.
    static const char names[] = "ABsSUDLR";
    uint8_t pad = 0;
    for (; *s; s++) {
        const char *p = strchr(names, *s);
        if (p)
            pad |= 1 << (p - names);
    }
    return pad;
}

static void load_input(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        exit(1);
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash)
            *hash = 0;
        long tick;
        char buttons[64] = "";
        if (sscanf(line, "%ld %63s", &tick, buttons) < 1)
            continue;
        if (!strcmp(buttons, "end")) {
            input_end = tick;
            continue;
        }
        events = realloc(events, (event_count + 1) * sizeof *events);
        events[event_count].tick = tick;
        events[event_count].pad = parse_buttons(buttons);
        event_count++;
    }
    fclose(f);
}

static uint8_t pad_for_tick(long tick)
{
    uint8_t pad = 0;
    for (int i = 0; i < event_count && events[i].tick <= tick; i++)
        pad = events[i].pad;
    return pad;
}

static int bank_of(uint16_t addr)
{
    return addr >= 0x8000 ? vnesPrgBank((addr - 0x8000) >> 13) : -1;
}

static void fail(const char *what)
{
    Cpu *c = &cur->cpu;
    fprintf(stderr, "nesref: %s: %s at %04X (bank %d, frame %ld, tick %ld)\n", cur->name, what,
            c->pc, bank_of(c->pc), vnes->frame, cur->tick);
    exit(1);
}

// Run one instruction, interrupt entry or hook on the current machine.
static void step(void)
{
    Cpu *c = &cur->cpu;
    if (vnes->nmi) {
        vnes->nmi = 0;
        c->nmi = 1;
    }
    c->irq = vnes->irq_pending;

    if (cur->hooks && !c->nmi && !(c->irq && !(c->p & FLAG_I))) {
        for (int i = 0; i < hook_count; i++) {
            const PortHook *h = &hooks[i];
            if (h->addr == c->pc && (h->bank < 0 || h->bank == bank_of(c->pc))) {
                hook_calls[i]++;
                h->fn(c);
                // Return to the caller.
                c->s++;
                c->pc = vnesRead(0x100 | c->s);
                c->s++;
                c->pc |= vnesRead(0x100 | c->s) << 8;
                c->pc++;
                return;
            }
        }
    }

    if (trace)
        printf("%-4s %6ld %04X A=%02X X=%02X Y=%02X S=%02X P=%02X\n", cur->name, vnes->frame,
               c->pc, c->a, c->x, c->y, c->s, c->p);
    int cyc = cpuStep(c);
    if (cyc < 0)
        fail("unknown opcode");
    vnesAddCycles(cyc);
}

Cpu *portCpu(void)
{
    return &cur->cpu;
}

void call6502(uint16_t addr)
{
    Cpu *c = &cur->cpu;
    uint8_t s = c->s;
    // Return address $FFFF + 1 = $0000, which the game never executes.
    vnesWrite(0x100 | c->s--, 0xFF);
    vnesWrite(0x100 | c->s--, 0xFF);
    c->pc = addr;
    while (!(c->pc == 0x0000 && c->s == s))
        step();
}

// Run the current machine until it reaches the end of its next tick.
static void run_tick(void)
{
    Cpu *c = &cur->cpu;
    vnes->pad = pad_for_tick(cur->tick);
    do
        step();
    while (c->pc != tick_pc);
    cur->tick++;
}

typedef struct {
    int lo, hi;
} Range;

int main(int argc, char **argv)
{
    const char *rom = NULL, *out = NULL, *input = NULL, *ppu_out = NULL;
    long max_ticks = -1;
    int speed = 1, compare = 0;
    Range ignore[16];
    int ignore_count = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tick-pc") && i + 1 < argc)
            tick_pc = strtol(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--ticks") && i + 1 < argc)
            max_ticks = atol(argv[++i]);
        else if (!strcmp(argv[i], "--input") && i + 1 < argc)
            input = argv[++i];
        else if (!strcmp(argv[i], "--speed") && i + 1 < argc)
            speed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--ppu") && i + 1 < argc)
            ppu_out = argv[++i];
        else if (!strcmp(argv[i], "--compare"))
            compare = 1;
        else if (!strcmp(argv[i], "--ignore") && i + 1 < argc && ignore_count < 16) {
            char *end;
            ignore[ignore_count].lo = strtol(argv[++i], &end, 16);
            ignore[ignore_count].hi = *end == '-' ? strtol(end + 1, NULL, 16) : ignore[ignore_count].lo;
            ignore_count++;
        } else if (!strcmp(argv[i], "--trace"))
            trace = 1;
        else if (!rom)
            rom = argv[i];
        else {
            fprintf(stderr, "nesref: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!rom || tick_pc < 0) {
        fprintf(stderr, "usage: nesref ROM --tick-pc ADDR [--ticks N] [--input FILE] [--speed N]\n"
                        "       [--compare] [--ignore A-B] [--out FILE] [--ppu FILE] [--trace]\n");
        return 2;
    }

    FILE *f = fopen(rom, "rb");
    if (!f) {
        perror(rom);
        return 1;
    }
    uint8_t header[16];
    if (fread(header, 1, 16, f) != 16 || memcmp(header, "NES\x1a", 4)) {
        fprintf(stderr, "nesref: %s is not an iNES file\n", rom);
        return 1;
    }
    int mapper = (header[6] >> 4) | (header[7] & 0xF0);
    if (mapper != 4) {
        fprintf(stderr, "nesref: mapper %d, only MMC3 (4) is supported\n", mapper);
        return 1;
    }
    if (header[6] & 4)
        fseek(f, 512, SEEK_CUR);
    int prg_size = header[4] * 0x4000, chr_size = header[5] * 0x2000;
    uint8_t *prg = malloc(prg_size), *chr = malloc(chr_size);
    if (fread(prg, 1, prg_size, f) != (size_t)prg_size || fread(chr, 1, chr_size, f) != (size_t)chr_size) {
        fprintf(stderr, "nesref: %s is truncated\n", rom);
        return 1;
    }
    fclose(f);
    if (input)
        load_input(input);
    if (max_ticks < 0)
        max_ticks = input_end >= 0 ? input_end : 600;
    if (&port_hook_count) {
        hooks = port_hooks;
        hook_count = port_hook_count;
    }
    hook_calls = calloc(hook_count + 1, sizeof *hook_calls);

    int count = compare ? 2 : 1;
    machines[0].name = "ref";
    machines[1].name = "port";
    machines[1].hooks = 1;
    for (int i = 0; i < count; i++) {
        Machine *m = &machines[i];
        use_machine(m);
        vnesInit(&m->nes, prg, prg_size, chr, chr_size);
        m->nes.dots_per_line = 341L * speed;
        m->cpu = (Cpu){ .read = bus_read, .write = bus_write };
        cpuReset(&m->cpu);
    }

    FILE *dump = out ? fopen(out, "wb") : NULL;
    int status = 0;
    for (long tick = 0; tick < max_ticks; tick++) {
        for (int i = 0; i < count; i++) {
            use_machine(&machines[i]);
            run_tick();
        }
        Machine *ref = &machines[0];
        if (dump) {
            uint32_t hdr[2] = { (uint32_t)tick, (uint32_t)ref->nes.frame };
            fwrite(hdr, 4, 2, dump);
            fwrite(ref->nes.ram, 1, sizeof ref->nes.ram, dump);
        }
        if (!compare)
            continue;
        const uint8_t *a = machines[0].nes.ram, *b = machines[1].nes.ram;
        int diffs = 0;
        for (int addr = 0; addr < 0x800; addr++) {
            int skip = 0;
            for (int r = 0; r < ignore_count; r++)
                skip |= addr >= ignore[r].lo && addr <= ignore[r].hi;
            if (skip || a[addr] == b[addr])
                continue;
            if (!diffs++)
                printf("tick %ld: state differs (ref / port)\n", tick);
            if (diffs <= 32)
                printf("  $%04X  %02X / %02X\n", addr, a[addr], b[addr]);
        }
        // PPU and mapper state.
        const Vnes *p = &machines[0].nes, *q = &machines[1].nes;
        struct {
            const char *name;
            const uint8_t *a, *b;
            int size;
        } areas[] = {
            { "vram", p->vram, q->vram, sizeof p->vram },
            { "palette", p->palette, q->palette, sizeof p->palette },
            { "oam", p->oam, q->oam, sizeof p->oam },
            { "mmc3 R", p->regs, q->regs, sizeof p->regs },
            { "mmc3 select", &p->bank_select, &q->bank_select, 1 },
            { "mirroring", &p->mirroring, &q->mirroring, 1 },
            { "irq latch", &p->irq_latch, &q->irq_latch, 1 },
            { "irq enabled", &p->irq_enabled, &q->irq_enabled, 1 },
            { "ppuctrl", &p->ctrl, &q->ctrl, 1 },
            { "ppumask", &p->mask, &q->mask, 1 },
        };
        for (size_t k = 0; k < sizeof areas / sizeof areas[0]; k++) {
            for (int i = 0; i < areas[k].size; i++) {
                if (areas[k].a[i] == areas[k].b[i])
                    continue;
                if (!diffs++)
                    printf("tick %ld: state differs (ref / port)\n", tick);
                if (diffs <= 32)
                    printf("  %s[%03X]  %02X / %02X\n", areas[k].name, i, areas[k].a[i], areas[k].b[i]);
            }
        }
        if (diffs) {
            if (diffs > 32)
                printf("  ... %d bytes in total\n", diffs);
            status = 1;
            break;
        }
    }

    if (dump)
        fclose(dump);
    if (ppu_out) {
        use_machine(&machines[0]);
        FILE *p = fopen(ppu_out, "wb");
        uint8_t nt[0x1000];
        for (int i = 0; i < 0x1000; i++)
            nt[i] = vnes->vram[vnesNametableIndex(0x2000 + i)];
        fwrite(nt, 1, sizeof nt, p);
        fwrite(vnes->palette, 1, 32, p);
        fwrite(vnes->oam, 1, 256, p);
        fwrite(vnes->regs, 1, 8, p);
        fclose(p);
    }
    fprintf(stderr, "nesref: %ld ticks, %ld frames\n", machines[0].tick, machines[0].nes.frame);
    if (compare) {
        for (int i = 0; i < hook_count; i++)
            fprintf(stderr, "  %-24s %ld calls\n", hooks[i].name, hook_calls[i]);
        if (!status)
            fprintf(stderr, "nesref: RAM and PPU state match on every tick\n");
    }
    return status;
}
