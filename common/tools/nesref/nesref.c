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
// palette, OAM), PPU/MMC3/APU registers and the sequence of APU writes are
// compared and the first difference
// is reported.
//
// Usage: nesref ROM --tick-pc ADDR [options]
//   --tick-pc ADDR   PC where a tick ends (RAM is compared / dumped there)
//   --ticks N        stop after N ticks (default 600)
//   --input FILE     input script, one "TICK BUTTONS" per line: from that
//                    tick on the pad holds BUTTONS (letters ABsSUDLR or "-");
//                    "TICK end" stops there unless --ticks is given;
//                    "TICK poke ADDR VALUE" (hex) writes RAM on both
//                    machines before that tick, e.g. to jump to a stage
//   --speed N        run the CPU N times faster than real hardware, so the
//                    original never lags (default 1; use 8 or more with
//                    --compare since hooks take no CPU time)
//   --compare        run ref and port and compare RAM every tick
//   --ignore A-B     RAM range left out of the comparison (repeatable)
//   --out FILE       binary RAM dump: per tick u32 tick, u32 frame, RAM[0x800]
//   --ppu FILE       write the PPU memory at the end (nametables 4K,
//                    palette 32, OAM 256, MMC3 R0-R7)
//   --idle PC:ADDR   the main loop waits at PC while RAM[ADDR] is 0; jump
//                    straight to the next interrupt there (only speeds up
//                    large --speed values)
//   --coverage FILE  append "name original-calls hook-calls" for every hook:
//                    how often the original routine was reached on the ref
//                    machine, and how often the C version ran through its
//                    hook on the port machine
//   --trace          print every instruction (slow)
//
// The pad is applied per tick, not per frame, so lag frames in the original
// do not shift the input.
#include "nesref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vnes.h"

// Hooks registered with PORT_HOOK; the section is empty (and the symbols
// null) when nesref is built on its own.
extern const PortHook *const __start_port_hooks[] __attribute__((weak));
extern const PortHook *const __stop_port_hooks[] __attribute__((weak));
static const PortHook *const *hooks;
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
static long idle_pc = -1, idle_addr;
static int trace;
static long *hook_calls;   // C runs on the port machine
static long *orig_calls;   // the original routine reached on the ref machine
static int16_t *hook_first; // per address: first hook index + 1, 0 = none
static int16_t *hook_next;  // next hook at the same address (other banks)

static int bank_of(uint16_t addr)
{
    return addr >= 0x8000 ? vnesPrgBank((addr - 0x8000) >> 13) : -1;
}

// The hook for the routine at the PC, or -1.
static int find_hook(uint16_t pc)
{
    for (int i = hook_first[pc] - 1; i >= 0; i = hook_next[i] - 1) {
        const PortHook *h = hooks[i];
        if (h->bank < 0 || h->bank == bank_of(pc))
            return i;
    }
    return -1;
}

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

typedef struct {
    long tick;
    uint16_t addr;
    uint8_t value;
} Poke;

static Poke *pokes;
static int poke_count;

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
        unsigned addr, value;
        if (!strcmp(buttons, "poke")) {
            if (sscanf(line, "%*d %*s %x %x", &addr, &value) != 2 || addr >= 0x800) {
                fprintf(stderr, "nesref: %s: bad poke: %s", path, line);
                exit(1);
            }
            pokes = realloc(pokes, (poke_count + 1) * sizeof *pokes);
            pokes[poke_count++] = (Poke){ tick, addr, value };
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

    int hi = hook_first[c->pc] && !c->nmi && !(c->irq && !(c->p & FLAG_I)) ? find_hook(c->pc) : -1;
    if (hi >= 0 && !cur->hooks)
        orig_calls[hi]++;
    if (hi >= 0 && cur->hooks) {
        const PortHook *h = hooks[hi];
        hook_calls[hi]++;
        h->fn(c);
        // Return to the caller (RTI also restores the flags).
        if (h->rti) {
            c->s++;
            c->p = (vnesRead(0x100 | c->s) & ~FLAG_B) | FLAG_U;
        }
        c->s++;
        c->pc = vnesRead(0x100 | c->s);
        c->s++;
        c->pc |= vnesRead(0x100 | c->s) << 8;
        if (!h->rti)
            c->pc++;
        return;
    }

    // Waiting for NMI: skip whole scanlines until an interrupt is due.
    if (c->pc == idle_pc && !vnes->ram[idle_addr] && !c->nmi && !c->irq) {
        while (!vnes->nmi && !vnes->irq_pending)
            vnesAddCycles((int)((vnes->dots_per_line - vnes->dots + 2) / 3));
        return;
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
    for (int i = 0; i < poke_count; i++)
        if (pokes[i].tick == cur->tick)
            vnes->ram[pokes[i].addr] = pokes[i].value;
    long steps = 0;
    do {
        step();
        // A tick normally takes well under 100k steps; this catches a port
        // that waits for something only the original would change.
        if (++steps > 5000000)
            fail("tick does not end (hang?)");
    } while (c->pc != tick_pc);
    cur->tick++;
}

typedef struct {
    int lo, hi;
} Range;

int main(int argc, char **argv)
{
    const char *rom = NULL, *out = NULL, *input = NULL, *ppu_out = NULL, *coverage = NULL;
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
        else if (!strcmp(argv[i], "--idle") && i + 1 < argc) {
            char *end;
            idle_pc = strtol(argv[++i], &end, 16);
            idle_addr = *end == ':' ? strtol(end + 1, NULL, 16) & 0x7FF : 0;
        } else if (!strcmp(argv[i], "--coverage") && i + 1 < argc)
            coverage = argv[++i];
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
    if (__start_port_hooks) {
        hooks = __start_port_hooks;
        hook_count = __stop_port_hooks - __start_port_hooks;
    }
    hook_calls = calloc(hook_count + 1, sizeof *hook_calls);
    orig_calls = calloc(hook_count + 1, sizeof *orig_calls);
    hook_first = calloc(0x10000, sizeof *hook_first);
    hook_next = calloc(hook_count + 1, sizeof *hook_next);
    for (int i = 0; i < hook_count; i++) {
        hook_next[i] = hook_first[hooks[i]->addr];
        hook_first[hooks[i]->addr] = i + 1;
    }

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
            { "apu", p->apu, q->apu, sizeof p->apu },
            { "apu write hash", (const uint8_t *)&p->apu_hash, (const uint8_t *)&q->apu_hash, 4 },
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
    if (coverage) {
        FILE *cov = fopen(coverage, "a");
        for (int i = 0; i < hook_count; i++)
            fprintf(cov, "%s %ld %ld\n", hooks[i]->name, orig_calls[i], hook_calls[i]);
        fclose(cov);
    }
    if (compare) {
        if (!status)
            fprintf(stderr, "nesref: RAM and PPU state match on every tick\n");
    }
    return status;
}
