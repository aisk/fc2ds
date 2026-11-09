// Headless reference runner for MMC3, NROM and UxROM NES games.
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
//                    large --speed values); --idle PC: always idle at PC
//   --tick-nmi       a tick ends at the tick PC only after an NMI was taken
//                    in it: for games that run everything in the NMI and
//                    idle in an empty loop (the tick PC is that loop)
//   --coverage FILE  append "name original-calls hook-calls" for every hook:
//                    how often the original routine was reached on the ref
//                    machine, and how often the C version ran through its
//                    hook on the port machine
//   --native         the port machine runs no 6502 code: the game's GameMain()
//                    (reset and main loop in C) drives it, interrupts call
//                    the hooks at the NMI / IRQ vectors directly, and
//                    call6502 only dispatches to hooks. It calls portTickEnd()
//                    at the end of each tick and portWaitNmi() to wait for
//                    vblank. Implies --compare.
//   --shot TICK:PREFIX  after that tick write PREFIX-ref.ppm (and
//                    PREFIX-port.ppm with --compare / --native): the last
//                    frame drawn by common/vnes/vrender (repeatable);
//                    with the game's GameScreenLayout, also PREFIX-*-nds.ppm:
//                    both NDS screens as common/nds ndsframe builds them
//   --pcs FILE       append "PC bank count" for every 6502 instruction the
//                    port machine still runs (what is left to port)
//   --wav FILE       synthesize the reference machine's sound with
//                    common/vnes/vapu (one NES frame of samples per frame,
//                    register writes applied as they happen) into a 16-bit
//                    mono WAV at the NDS stream rate
//   --trace          print every instruction (slow)
//
// The pad is applied per tick, not per frame, so lag frames in the original
// do not shift the input.
#include "nesref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vnes.h"
#include "vrender.h"
#include "vapu.h"
#include "ndsframe.h"
#include "nes_chr.h"

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
    int nmi_seen; // an NMI was taken during the current tick
} Machine;

static Machine machines[2];
static Machine *cur;
static long tick_pc = -1;
static long idle_pc = -1, idle_addr;
static int tick_nmi;
static int trace;
static int native;
static long *port_pcs; // per (bank, PC): 6502 instructions run on the port machine
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
        cur->nmi_seen = 1;
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
    if (c->pc == idle_pc && (idle_addr < 0 || !vnes->ram[idle_addr]) && !c->nmi && !c->irq) {
        while (!vnes->nmi && !vnes->irq_pending)
            vnesAddCycles((int)((vnes->dots_per_line - vnes->dots + 2) / 3));
        return;
    }

    if (trace)
        printf("%-4s %6ld %04X A=%02X X=%02X Y=%02X S=%02X P=%02X\n", cur->name, vnes->frame,
               c->pc, c->a, c->x, c->y, c->s, c->p);
    if (port_pcs && cur->hooks)
        port_pcs[(bank_of(c->pc) + 1) * 0x10000L + c->pc]++;
    int cyc = cpuStep(c);
    if (cyc < 0)
        fail("unknown opcode");
    vnesAddCycles(cyc);
}

Cpu *portCpu(void)
{
    return &cur->cpu;
}

// Run a hook directly, as if the CPU had called it with JSR: the wrapper
// may pop the return address of its caller (c->s += 2), which a C caller
// cannot follow, so that is an error.
static void run_hook_direct(int hi, uint16_t addr)
{
    Cpu *c = &cur->cpu;
    const PortHook *h = hooks[hi];
    uint8_t s = c->s;
    vnesWrite(0x100 | c->s--, 0xFF);
    vnesWrite(0x100 | c->s--, 0xFF);
    hook_calls[hi]++;
    c->pc = addr;
    h->fn(c);
    if (c->s != (uint8_t)(s - 2)) {
        c->pc = addr;
        fail("hook returned past its C caller");
    }
    c->s = s;
}

void call6502(uint16_t addr)
{
    Cpu *c = &cur->cpu;
    if (native) {
        int hi = hook_first[addr] ? find_hook(addr) : -1;
        if (hi < 0) {
            c->pc = addr;
            fail("call6502 to a routine with no hook (--native)");
        }
        run_hook_direct(hi, addr);
        return;
    }
    uint8_t s = c->s;
    // Return address $FFFF + 1 = $0000, which the game never executes.
    vnesWrite(0x100 | c->s--, 0xFF);
    vnesWrite(0x100 | c->s--, 0xFF);
    c->pc = addr;
    while (!(c->pc == 0x0000 && c->s == s))
        step();
}

// Take one pending interrupt on the port machine.
static void take_interrupt(int is_nmi)
{
    Cpu *c = &cur->cpu;
    if (native) {
        uint16_t vec = is_nmi ? 0xFFFA : 0xFFFE;
        uint16_t addr = vnesRead(vec) | vnesRead(vec + 1) << 8;
        if (is_nmi)
            vnes->nmi = 0;
        int hi = find_hook(addr);
        if (hi < 0) {
            c->pc = addr;
            fail("interrupt handler has no hook (--native)");
        }
        hook_calls[hi]++;
        hooks[hi]->fn(c);
        return;
    }
    // Park the CPU at $0000 (never executed): the interrupt pushes it and
    // the handler's RTI comes back to it.
    uint16_t pc = c->pc;
    uint8_t s = c->s;
    c->pc = 0x0000;
    do
        step();
    while (!(c->pc == 0x0000 && c->s == s));
    c->pc = pc;
}

// Called by ported code where the original waits for something only the
// NMI changes: let time pass until the next NMI has been handled, taking
// the IRQs that come first.
void portWaitNmi(void)
{
    Cpu *c = &cur->cpu;
    for (;;) {
        if (vnes->nmi) {
            take_interrupt(1);
            return;
        }
        if (vnes->irq_pending && (native || !(c->p & FLAG_I)))
            take_interrupt(0);
        else if (native)
            vnesRunUntil(-1); // as on the NDS
        else
            vnesAddCycles((int)((vnes->dots_per_line - vnes->dots + 2) / 3));
    }
}

// --wav: sound of the reference machine.
#define WAV_RATE 32768
static FILE *wav;
static long wav_samples, wav_frame;
static uint32_t wav_frac;

static void wav_frame_done(void)
{
    // NES frames are 29780.5 CPU cycles: 60.0988 per second.
    int16_t buf[WAV_RATE / 50];
    wav_frac += (uint32_t)((uint64_t)WAV_RATE * 29780500 / VAPU_CPU_HZ);
    int n = (int)(wav_frac / 1000);
    wav_frac %= 1000;
    vapuRender(buf, n);
    fwrite(buf, 2, n, wav);
    wav_samples += n;
}

static void wav_header(uint32_t samples)
{
    uint32_t data = samples * 2;
    uint8_t h[45] = "RIFF....WAVEfmt \x10\0\0\0\1\0\1\0........\2\0\x10\0data....";
    uint32_t v[4] = { 36 + data, WAV_RATE, WAV_RATE * 2, data };
    int at[4] = { 4, 24, 28, 40 };
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 4; b++)
            h[at[i] + b] = (uint8_t)(v[i] >> (8 * b));
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
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
        if (wav && cur == &machines[0] && vnes->frame != wav_frame) {
            wav_frame = vnes->frame;
            wav_frame_done();
        }
        // A tick normally takes well under 100k steps; this catches a port
        // that waits for something only the original would change.
        if (++steps > 5000000)
            fail("tick does not end (hang?)");
    } while (c->pc != tick_pc || (tick_nmi && !cur->nmi_seen));
    cur->nmi_seen = 0;
    cur->tick++;
}

typedef struct {
    int lo, hi;
} Range;

static Range ignore[16];
static int ignore_count;
static long max_ticks = -1;
static FILE *dump;
static const char *ppu_out, *coverage, *pcs_out;
static int compare;

// The game's reset and main loop in C (--native); see portTickEnd.
void GameMain(void) __attribute__((weak));

typedef struct {
    long tick;
    const char *prefix;
} Shot;

static Shot shots[64];
static int shot_count;

static void write_ppm(const char *path)
{
    static VrasterFrame frame;
    static uint8_t pix[240][256];
    vrasterBuild(&frame);
    vrenderFrame(&frame, pix);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n256 240\n255\n");
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 256; x++) {
            uint32_t c = vrenderRgb(pix[y][x]);
            uint8_t rgb[3] = { c >> 16, c >> 8, c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

void GameScreenLayout(const VrasterFrame *f, NdsLayout *out) __attribute__((weak));

// The current machine's frame as the NDS shows it (top over bottom).
static void write_nds_ppm(const char *path)
{
    static VrasterFrame frame;
    static NdsScreen top, bot;
    static NdsTileCache bg, obj, bg2, obj2;
    static uint32_t *chr4;
    static uint8_t pix[2][192][256];
    if (!chr4 || vnes->chr_is_ram) {
        if (!chr4)
            chr4 = malloc((size_t)vnes->chr_size * 2);
        nesChrTo4bpp(vnes->chr, chr4, vnes->chr_size / 16);
    }
    ndsTileCacheInit(&bg);
    ndsTileCacheInit(&obj);
    ndsTileCacheInit(&bg2);
    ndsTileCacheInit(&obj2);
    vrasterBuild(&frame);
    NdsLayout lay = { 0 };
    GameScreenLayout(&frame, &lay);
    ndsScreenBuild(&top, &bg, &obj, &frame, lay.top_first, lay.top_end, 0, 1);
    top.right = 256 - lay.top_crop_right;
    ndsScreenCompose(&top, &bg, &obj, chr4, pix[0]);
    if (lay.bot_end > lay.bot_first) {
        ndsScreenBuild(&bot, &bg2, &obj2, &frame, lay.bot_first, lay.bot_end, lay.bot_dst, 0);
        ndsScreenCompose(&bot, &bg2, &obj2, chr4, pix[1]);
    } else {
        memset(pix[1], 0x0F, sizeof pix[1]);
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P6\n256 384\n255\n");
    for (int y = 0; y < 384; y++)
        for (int x = 0; x < 256; x++) {
            uint32_t c = vrenderRgb(pix[y / 192][y % 192][x]);
            uint8_t rgb[3] = { c >> 16, c >> 8, c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

// Screenshots requested for this tick.
static void take_shots(long tick)
{
    Machine *keep = cur;
    for (int i = 0; i < shot_count; i++) {
        if (shots[i].tick != tick)
            continue;
        for (int m = 0; m < (compare ? 2 : 1); m++) {
            char path[512];
            snprintf(path, sizeof path, "%s-%s.ppm", shots[i].prefix, machines[m].name);
            use_machine(&machines[m]);
            write_ppm(path);
            if (GameScreenLayout) {
                snprintf(path, sizeof path, "%s-%s-nds.ppm", shots[i].prefix, machines[m].name);
                write_nds_ppm(path);
            }
        }
    }
    use_machine(keep);
}

// Compare the machines after a tick; returns 1 on a difference.
static int compare_tick(long tick)
{
    take_shots(tick);
    Machine *ref = &machines[0];
    if (dump) {
        uint32_t hdr[2] = { (uint32_t)tick, (uint32_t)ref->nes.frame };
        fwrite(hdr, 4, 2, dump);
        fwrite(ref->nes.ram, 1, sizeof ref->nes.ram, dump);
    }
    if (!compare)
        return 0;
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
        { "prg page", &p->prg_page, &q->prg_page, 1 },
        { "chr ram", p->chr_ram, q->chr_ram, p->chr_is_ram ? (int)sizeof p->chr_ram : 0 },
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
    if (diffs > 32)
        printf("  ... %d bytes in total\n", diffs);
    return diffs != 0;
}

// Output files and the summary; returns the exit status.
static int finish(int status)
{
    if (dump)
        fclose(dump);
    if (wav) {
        wav_header((uint32_t)wav_samples);
        fclose(wav);
    }
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
    if (port_pcs) {
        FILE *pf = fopen(pcs_out, "a");
        for (long i = 0; i < 64 * 0x10000L; i++)
            if (port_pcs[i])
                fprintf(pf, "%04lX %ld %ld\n", i & 0xFFFF, (i >> 16) - 1, port_pcs[i]);
        fclose(pf);
    }
    if (compare && !status)
        fprintf(stderr, "nesref: RAM and PPU state match on every tick\n");
    return status;
}

// Input and pokes for the port machine's next tick (--native).
static void start_port_tick(void)
{
    vnes->pad = pad_for_tick(cur->tick);
    for (int i = 0; i < poke_count; i++)
        if (pokes[i].tick == cur->tick)
            vnes->ram[pokes[i].addr] = pokes[i].value;
}

// Subtitles are an NDS feature; nothing to do here.
void portMessage(int id)
{
    (void)id;
}

// --native: the port's main loop calls this where the original reaches the
// tick PC. Runs the reference machine through the same tick, compares, and
// exits when the run is over or the state differs.
void portTickEnd(void)
{
    Machine *port = cur;
    port->tick++;
    use_machine(&machines[0]);
    run_tick();
    int status = compare_tick(machines[0].tick - 1);
    if (status || machines[0].tick >= max_ticks)
        exit(finish(status));
    use_machine(port);
    start_port_tick();
}


int main(int argc, char **argv)
{
    const char *rom = NULL, *out = NULL, *input = NULL;
    int speed = 1;
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
            idle_addr = *end == ':' && end[1] ? strtol(end + 1, NULL, 16) & 0x7FF : -1;
        } else if (!strcmp(argv[i], "--coverage") && i + 1 < argc)
            coverage = argv[++i];
        else if (!strcmp(argv[i], "--tick-nmi"))
            tick_nmi = 1;
        else if (!strcmp(argv[i], "--compare"))
            compare = 1;
        else if (!strcmp(argv[i], "--native"))
            native = compare = 1;
        else if (!strcmp(argv[i], "--ignore") && i + 1 < argc && ignore_count < 16) {
            char *end;
            ignore[ignore_count].lo = strtol(argv[++i], &end, 16);
            ignore[ignore_count].hi = *end == '-' ? strtol(end + 1, NULL, 16) : ignore[ignore_count].lo;
            ignore_count++;
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc && shot_count < 64) {
            char *end;
            shots[shot_count].tick = strtol(argv[++i], &end, 10);
            shots[shot_count++].prefix = *end == ':' ? end + 1 : "shot";
        } else if (!strcmp(argv[i], "--pcs") && i + 1 < argc)
            pcs_out = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) {
            if (!(wav = fopen(argv[++i], "wb"))) {
                perror(argv[i]);
                return 1;
            }
            wav_header(0);
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
    if (mapper != 4 && mapper != 0 && mapper != 2) {
        fprintf(stderr, "nesref: mapper %d, only MMC3 (4), NROM (0) and UxROM (2) are supported\n", mapper);
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

    if (pcs_out)
        port_pcs = calloc(64 * 0x10000L, sizeof *port_pcs);

    int count = compare ? 2 : 1;
    machines[0].name = "ref";
    machines[1].name = "port";
    machines[1].hooks = 1;
    for (int i = 0; i < count; i++) {
        Machine *m = &machines[i];
        use_machine(m);
        vnesInit(&m->nes, prg, prg_size, chr, chr_size);
        if (mapper != 4) {
            m->nes.mapper = mapper == 2 ? VNES_UXROM : VNES_NROM;
            m->nes.mirroring = header[6] & 1 ? 0 : 1; // iNES bit 0: vertical
        }
        m->nes.dots_per_line = 341L * speed;
        m->cpu = (Cpu){ .read = bus_read, .write = bus_write };
        cpuReset(&m->cpu);
    }
    if (wav) {
        vapuInit(WAV_RATE);
        vapuSetMemory(vnesRead);
        machines[0].nes.apu_write = vapuWrite;
    }

    dump = out ? fopen(out, "wb") : NULL;
    if (native) {
        if (!GameMain) {
            fprintf(stderr, "nesref: --native needs the game's GameMain()\n");
            return 2;
        }
        use_machine(&machines[1]);
        start_port_tick();
        GameMain(); // returns only through exit() in portTickEnd
        return 1;
    }
    int status = 0;
    for (long tick = 0; tick < max_ticks && !status; tick++) {
        for (int i = 0; i < count; i++) {
            use_machine(&machines[i]);
            run_tick();
        }
        status = compare_tick(tick);
    }
    return finish(status);
}
