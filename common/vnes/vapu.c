#include "vapu.h"

#include <string.h>

// The sample loop runs ~33k times a second: on the NDS keep it in ITCM as
// ARM code and its state in DTCM, away from main RAM wait states.
#if defined(ARM9)
#include <nds/ndstypes.h>
#define VAPU_HOT ITCM_CODE ARM_CODE
#define VAPU_STATE DTCM_BSS
#else
#define VAPU_HOT
#define VAPU_STATE
#endif

static const uint8_t length_table[32] = {
    10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
    12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint8_t duty_table[4] = { 0x02, 0x06, 0x1E, 0xF9 }; // bit n = step n
static const uint16_t noise_period[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};
static const uint16_t dmc_period[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54,
};

// DMC sample playback: the memory reader fills a one byte buffer, the
// output unit shifts its bits out, one per period, each moving the level
// up or down by 2.
typedef struct {
    uint8_t loop, rate;
    uint16_t start, length;      // from $4012/$4013
    uint16_t addr, remaining;    // current sample read
    uint8_t buffer, buffer_full;
    uint8_t shift, bits, silent;
    int32_t timer;
} Dmc;

static uint8_t (*dmc_read)(uint16_t addr);

typedef struct {
    uint8_t start, loop, constant, volume; // volume = V (envelope period / constant volume)
    uint8_t divider, decay;
} Envelope;

typedef struct {
    int32_t timer;  // CPU cycles until the next step
    int32_t idle;   // cycles not yet run while the channel was silent
    int32_t period; // CPU cycles per step
    uint8_t step;
    uint8_t level;  // current output (0-15)
    uint8_t vol;    // output when the waveform is high, 0 when silenced
    uint8_t duty;   // pulse: duty_table entry
    uint8_t enabled, length;
    Envelope env;
    uint16_t raw;   // pulse/triangle: 11-bit period register
    // pulse sweep
    uint8_t sweep_on, sweep_period, sweep_neg, sweep_shift, sweep_div, sweep_reload;
    // triangle
    uint8_t linear, linear_reload_value, linear_reload, control;
    // noise
    uint8_t mode;
    uint16_t lfsr;
} Channel;

static struct {
    Channel ch[4]; // pulse 1, pulse 2, triangle, noise
    int five_step, frame_step;
    int32_t frame_cycle, next_event;
    uint32_t cycle_frac, cycles_per_sample_fp; // 16.16 CPU cycles per sample
    int32_t hp_prev_in, hp_prev_out, lp;
    int16_t pulse_mix[31];
    int16_t tnd_mix[203];
    uint8_t dmc_level; // DMC output (0-127): $4011, then the samples
    Dmc dmc;
} apu VAPU_STATE;

// Frame counter events in CPU cycles (NTSC), 4-step and 5-step mode: four
// quarter frames, then the end of the sequence.
static const int32_t frame_events[2][5] = {
    { 7457, 14913, 22371, 29829, 29830 },
    { 7457, 14913, 22371, 37281, 37282 },
};

static void env_clock(Envelope *e)
{
    if (e->start) {
        e->start = 0;
        e->decay = 15;
        e->divider = e->volume;
    } else if (e->divider) {
        e->divider--;
    } else {
        e->divider = e->volume;
        if (e->decay)
            e->decay--;
        else if (e->loop)
            e->decay = 15;
    }
}

static int env_out(const Envelope *e)
{
    return e->constant ? e->volume : e->decay;
}

static int sweep_target(const Channel *c, int ones_complement)
{
    int delta = c->raw >> c->sweep_shift;
    if (c->sweep_neg)
        return c->raw - delta - ones_complement;
    return c->raw + delta;
}

static int pulse_muted(const Channel *c, int idx)
{
    return c->raw < 8 || sweep_target(c, idx == 0) > 0x7FF;
}

// Run a timer that went to zero or below: the number of steps it took.
// Silenced channels may sit at ultrasonic periods (one step every few CPU
// cycles), so long runs are divided out instead of looped.
static inline int advance(int32_t *timer, int32_t period)
{
    int32_t t = *timer + period;
    int n = 1;
    if (t <= 0) {
        int k = -t / period + 1;
        n += k;
        t += k * period;
    }
    *timer = t;
    return n;
}

// Step the noise LFSR n times. Within a run of k steps every feedback bit
// still comes from bits of the starting value (tap 1: k <= 14, tap 6:
// k <= 9), so k steps are one shift and one xor.
static inline uint32_t lfsr_steps(uint32_t s, int n, int mode)
{
    int tap = mode ? 6 : 1, max = mode ? 9 : 14;
    while (n > 0) {
        int k = n < max ? n : max;
        uint32_t fb = (s ^ s >> tap) & ((1u << k) - 1);
        s = s >> k | fb << (15 - k);
        n -= k;
    }
    return s;
}

// A silent channel's timer is only counted in the sample loop: run it now,
// before anything can make the channel audible. Its output was 0 all
// along, so this gives the same steps and noise sequence as running it
// sample by sample.
static void catch_up(void)
{
    for (int i = 0; i < 4; i++) {
        Channel *c = &apu.ch[i];
        if (!c->idle)
            continue;
        c->timer -= c->idle;
        c->idle = 0;
        if (c->timer > 0)
            continue;
        int n = advance(&c->timer, c->period);
        if (i < 2)
            c->step = (c->step + n) & 7;
        else if (i == 3)
            c->lfsr = (uint16_t)lfsr_steps(c->lfsr, n, c->mode);
        // the triangle only steps while audible
    }
}

// Recompute what depends on registers and counters rather than on the
// waveform step: run after every write and frame counter event, so the
// sample loop only has to step the waveforms.
static void update(void)
{
    for (int i = 0; i < 2; i++) {
        Channel *c = &apu.ch[i];
        c->period = (c->raw + 1) * 2;
        c->vol = c->length && !pulse_muted(c, i) ? (uint8_t)env_out(&c->env) : 0;
        c->level = (c->duty >> c->step & 1) ? c->vol : 0;
    }
    Channel *t = &apu.ch[2];
    t->period = t->raw + 1;
    // Ultrasonic periods (< 2) would only alias: the triangle holds.
    t->vol = t->length && t->linear && t->raw >= 2;
    Channel *n = &apu.ch[3];
    n->vol = n->length ? (uint8_t)env_out(&n->env) : 0;
    n->level = (n->lfsr & 1) ? 0 : n->vol;
}

static void quarter_frame(void)
{
    env_clock(&apu.ch[0].env);
    env_clock(&apu.ch[1].env);
    env_clock(&apu.ch[3].env);
    Channel *t = &apu.ch[2];
    if (t->linear_reload)
        t->linear = t->linear_reload_value;
    else if (t->linear)
        t->linear--;
    if (!t->control)
        t->linear_reload = 0;
}

static void half_frame(void)
{
    for (int i = 0; i < 4; i++) {
        Channel *c = &apu.ch[i];
        int halt = i == 2 ? c->control : c->env.loop;
        if (!halt && c->length)
            c->length--;
    }
    for (int i = 0; i < 2; i++) {
        Channel *c = &apu.ch[i];
        if (!c->sweep_div && c->sweep_on && c->sweep_shift && !pulse_muted(c, i))
            c->raw = (uint16_t)sweep_target(c, i == 0);
        if (!c->sweep_div || c->sweep_reload) {
            c->sweep_div = c->sweep_period;
            c->sweep_reload = 0;
        } else {
            c->sweep_div--;
        }
    }
}

// Quarter frames at the first four events, half frames at the second and
// fourth, then the sequence restarts.
static void frame_event(void)
{
    catch_up();
    const int32_t *ev = frame_events[apu.five_step];
    while (apu.frame_cycle >= apu.next_event) {
        int i = apu.frame_step;
        if (i < 4) {
            quarter_frame();
            if (i == 1 || i == 3)
                half_frame();
            apu.frame_step++;
        } else {
            apu.frame_cycle -= ev[4];
            apu.frame_step = 0;
        }
        apu.next_event = ev[apu.frame_step];
    }
    update();
}

void vapuWrite(uint16_t addr, uint8_t v)
{
    int r = addr - 0x4000;
    if (r < 0 || r > 0x17)
        return;
    catch_up();
    if (r == 0x10) {
        apu.dmc.loop = (v >> 6) & 1;
        apu.dmc.rate = v & 15;
        return;
    }
    if (r == 0x12) {
        apu.dmc.start = (uint16_t)(0xC000 + v * 64);
        return;
    }
    if (r == 0x13) {
        apu.dmc.length = (uint16_t)(v * 16 + 1);
        return;
    }
    if (r == 0x11) {
        // DMC output level, set directly: it adds to the triangle/noise
        // DAC, so it also changes their loudness (games write it for that).
        apu.dmc_level = v & 0x7F;
        return;
    }
    if (r < 8) {
        Channel *c = &apu.ch[r >> 2];
        switch (r & 3) {
        case 0:
            c->duty = duty_table[v >> 6];
            c->env.loop = (v >> 5) & 1;
            c->env.constant = (v >> 4) & 1;
            c->env.volume = v & 15;
            break;
        case 1:
            c->sweep_on = v >> 7;
            c->sweep_period = (v >> 4) & 7;
            c->sweep_neg = (v >> 3) & 1;
            c->sweep_shift = v & 7;
            c->sweep_reload = 1;
            break;
        case 2:
            c->raw = (c->raw & 0x700) | v;
            break;
        case 3:
            c->raw = (uint16_t)((c->raw & 0xFF) | (v & 7) << 8);
            if (c->enabled)
                c->length = length_table[v >> 3];
            c->step = 0;
            c->env.start = 1;
            break;
        }
        update();
        return;
    }
    Channel *t = &apu.ch[2], *n = &apu.ch[3];
    switch (r) {
    case 0x08:
        t->control = v >> 7;
        t->linear_reload_value = v & 0x7F;
        break;
    case 0x0A:
        t->raw = (t->raw & 0x700) | v;
        break;
    case 0x0B:
        t->raw = (uint16_t)((t->raw & 0xFF) | (v & 7) << 8);
        if (t->enabled)
            t->length = length_table[v >> 3];
        t->linear_reload = 1;
        break;
    case 0x0C:
        n->env.loop = (v >> 5) & 1;
        n->env.constant = (v >> 4) & 1;
        n->env.volume = v & 15;
        break;
    case 0x0E:
        n->mode = v >> 7;
        n->period = noise_period[v & 15];
        break;
    case 0x0F:
        if (n->enabled)
            n->length = length_table[v >> 3];
        n->env.start = 1;
        break;
    case 0x15:
        for (int i = 0; i < 4; i++) {
            apu.ch[i].enabled = (v >> i) & 1;
            if (!apu.ch[i].enabled)
                apu.ch[i].length = 0;
        }
        if (!(v & 0x10)) {
            apu.dmc.remaining = 0;
        } else if (!apu.dmc.remaining) {
            apu.dmc.addr = apu.dmc.start;
            apu.dmc.remaining = apu.dmc.length;
        }
        break;
    case 0x17:
        apu.five_step = v >> 7;
        apu.frame_cycle = 0;
        apu.frame_step = 0;
        apu.next_event = frame_events[apu.five_step][0];
        if (apu.five_step) {
            quarter_frame();
            half_frame();
        }
        break;
    default:
        return;
    }
    update();
}

void vapuInit(int sample_rate)
{
    memset(&apu, 0, sizeof apu);
    apu.ch[3].lfsr = 1;
    apu.ch[3].period = noise_period[0];
    apu.dmc.silent = 1;
    apu.dmc.timer = dmc_period[0];
    apu.next_event = frame_events[0][0];
    apu.cycles_per_sample_fp = (uint32_t)(((uint64_t)VAPU_CPU_HZ << 16) / (uint32_t)sample_rate);
    // NES DAC curves (nesdev wiki "APU Mixer"), scaled so that the loudest
    // mix stays inside int16.
    for (int i = 1; i < 31; i++)
        apu.pulse_mix[i] = (int16_t)(95.52 / (8128.0 / i + 100) * 28000);
    for (int i = 1; i < 203; i++)
        apu.tnd_mix[i] = (int16_t)(163.67 / (24329.0 / i + 100) * 28000);
    update();
}

void vapuSetMemory(uint8_t (*read)(uint16_t addr))
{
    dmc_read = read;
}

// Run the DMC for `cycles` CPU cycles.
static void dmc_run(int cycles)
{
    Dmc *d = &apu.dmc;
    d->timer -= cycles;
    while (d->timer <= 0) {
        d->timer += dmc_period[d->rate];
        if (!d->silent) {
            if (d->shift & 1) {
                if (apu.dmc_level <= 125)
                    apu.dmc_level += 2;
            } else if (apu.dmc_level >= 2) {
                apu.dmc_level -= 2;
            }
            d->shift >>= 1;
        }
        if (d->bits)
            d->bits--;
        if (!d->bits) {
            d->bits = 8;
            d->silent = !d->buffer_full;
            d->shift = d->buffer;
            d->buffer_full = 0;
        }
        if (!d->buffer_full && d->remaining) {
            d->buffer = dmc_read ? dmc_read(d->addr) : 0x55; // 0x55: level stays
            d->buffer_full = 1;
            d->addr = d->addr == 0xFFFF ? 0x8000 : d->addr + 1;
            if (!--d->remaining && d->loop) {
                d->addr = d->start;
                d->remaining = d->length;
            }
        }
    }
}

static const uint8_t triangle_wave[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0,  1,  2,  3,  4,  5,  6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

VAPU_HOT void vapuRender(int16_t *out, int samples)
{
    Channel *p1 = &apu.ch[0], *p2 = &apu.ch[1], *t = &apu.ch[2], *n = &apu.ch[3];
    for (int s = 0; s < samples; s++) {
        apu.cycle_frac += apu.cycles_per_sample_fp;
        int cycles = (int)(apu.cycle_frac >> 16);
        apu.cycle_frac &= 0xFFFF;

        apu.frame_cycle += cycles;
        if (apu.frame_cycle >= apu.next_event)
            frame_event();

        if (!p1->vol)
            p1->idle += cycles;
        else if ((p1->timer -= cycles) <= 0) {
            p1->step = (p1->step + advance(&p1->timer, p1->period)) & 7;
            p1->level = (p1->duty >> p1->step & 1) ? p1->vol : 0;
        }
        if (!p2->vol)
            p2->idle += cycles;
        else if ((p2->timer -= cycles) <= 0) {
            p2->step = (p2->step + advance(&p2->timer, p2->period)) & 7;
            p2->level = (p2->duty >> p2->step & 1) ? p2->vol : 0;
        }
        if (!t->vol)
            t->idle += cycles;
        else if ((t->timer -= cycles) <= 0)
            t->step = (t->step + advance(&t->timer, t->period)) & 31;
        if (!n->vol)
            n->idle += cycles;
        else if ((n->timer -= cycles) <= 0) {
            uint32_t lfsr = lfsr_steps(n->lfsr, advance(&n->timer, n->period), n->mode);
            n->lfsr = (uint16_t)lfsr;
            n->level = (lfsr & 1) ? 0 : n->vol;
        }

        if (apu.dmc.remaining || apu.dmc.buffer_full || !apu.dmc.silent)
            dmc_run(cycles);

        int32_t x = apu.pulse_mix[p1->level + p2->level] + apu.tnd_mix[3 * triangle_wave[t->step] + 2 * n->level + apu.dmc_level];

        // ~90 Hz high-pass (removes the DC offset of the DAC curves) and a
        // light low-pass against aliasing, as on the console. Division
        // rounds to zero, so no DC is left when the input stops.
        int32_t hp = x - apu.hp_prev_in + apu.hp_prev_out * 251 / 256;
        apu.hp_prev_in = x;
        apu.hp_prev_out = hp;
        apu.lp += (hp - apu.lp) / 2;
        int32_t y = apu.lp;
        out[s] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
}
