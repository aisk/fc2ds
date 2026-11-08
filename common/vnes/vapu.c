#include "vapu.h"

#include <string.h>

static const uint8_t length_table[32] = {
    10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
    12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};
static const uint8_t duty_table[4][8] = {
    { 0, 1, 0, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 0, 0, 0, 0, 0 },
    { 0, 1, 1, 1, 1, 0, 0, 0 },
    { 1, 0, 0, 1, 1, 1, 1, 1 },
};
static const uint16_t noise_period[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068,
};

typedef struct {
    uint8_t start, loop, constant, volume; // volume = V (envelope period / constant volume)
    uint8_t divider, decay;
} Envelope;

typedef struct {
    int enabled;
    uint8_t length;
    Envelope env;
    // pulse
    uint8_t duty, step;
    uint16_t period;
    uint8_t sweep_on, sweep_period, sweep_neg, sweep_shift, sweep_div, sweep_reload;
    // triangle
    uint8_t linear, linear_reload_value, linear_reload, control;
    // noise
    uint8_t mode;
    uint16_t lfsr;
    int32_t timer; // CPU cycles until the next step
} Channel;

static struct {
    Channel ch[4]; // pulse 1, pulse 2, triangle, noise
    int five_step;
    int32_t frame_cycle;
    uint32_t cycle_frac, cycles_per_sample_fp; // 16.16 CPU cycles per sample
    int16_t pulse_mix[31];
    int16_t tnd_mix[203];
    int32_t hp_prev_in, hp_prev_out, lp;
} apu;

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
    int delta = c->period >> c->sweep_shift;
    if (c->sweep_neg)
        return c->period - delta - ones_complement;
    return c->period + delta;
}

static int pulse_muted(const Channel *c, int idx)
{
    return c->period < 8 || sweep_target(c, idx == 0) > 0x7FF;
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
            c->period = (uint16_t)sweep_target(c, i == 0);
        if (!c->sweep_div || c->sweep_reload) {
            c->sweep_div = c->sweep_period;
            c->sweep_reload = 0;
        } else {
            c->sweep_div--;
        }
    }
}

// Frame counter in CPU cycles (NTSC).
static void frame_counter(int cycles)
{
    int32_t before = apu.frame_cycle;
    apu.frame_cycle += cycles;
    static const int32_t q4[4] = { 7457, 14913, 22371, 29829 };
    static const int32_t q5[4] = { 7457, 14913, 22371, 37281 };
    const int32_t *q = apu.five_step ? q5 : q4;
    int32_t len = apu.five_step ? 37282 : 29830;
    for (int i = 0; i < 4; i++) {
        if (before < q[i] && apu.frame_cycle >= q[i]) {
            quarter_frame();
            if (i == 1 || i == 3)
                half_frame();
        }
    }
    if (apu.frame_cycle >= len)
        apu.frame_cycle -= len;
}

void vapuWrite(uint16_t addr, uint8_t v)
{
    int r = addr - 0x4000;
    if (r < 0 || r > 0x17)
        return;
    if (r < 8) {
        Channel *c = &apu.ch[r >> 2];
        switch (r & 3) {
        case 0:
            c->duty = v >> 6;
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
            c->period = (c->period & 0x700) | v;
            break;
        case 3:
            c->period = (uint16_t)((c->period & 0xFF) | (v & 7) << 8);
            if (c->enabled)
                c->length = length_table[v >> 3];
            c->step = 0;
            c->env.start = 1;
            break;
        }
        return;
    }
    Channel *t = &apu.ch[2], *n = &apu.ch[3];
    switch (r) {
    case 0x08:
        t->control = v >> 7;
        t->linear_reload_value = v & 0x7F;
        break;
    case 0x0A:
        t->period = (t->period & 0x700) | v;
        break;
    case 0x0B:
        t->period = (uint16_t)((t->period & 0xFF) | (v & 7) << 8);
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
        break;
    case 0x17:
        apu.five_step = v >> 7;
        apu.frame_cycle = 0;
        if (apu.five_step) {
            quarter_frame();
            half_frame();
        }
        break;
    }
}

void vapuInit(int sample_rate)
{
    memset(&apu, 0, sizeof apu);
    apu.ch[3].lfsr = 1;
    apu.ch[3].period = noise_period[0];
    apu.cycles_per_sample_fp = (uint32_t)(((uint64_t)VAPU_CPU_HZ << 16) / (uint32_t)sample_rate);
    // NES DAC curves (nesdev wiki "APU Mixer"), scaled so that the loudest
    // mix stays inside int16.
    for (int i = 1; i < 31; i++)
        apu.pulse_mix[i] = (int16_t)(95.52 / (8128.0 / i + 100) * 28000);
    for (int i = 1; i < 203; i++)
        apu.tnd_mix[i] = (int16_t)(163.67 / (24329.0 / i + 100) * 28000);
}

// Advance a channel's timer by `cycles` CPU cycles.
static void run_pulse(Channel *c, int cycles)
{
    c->timer -= cycles;
    int32_t step = (c->period + 1) * 2;
    while (c->timer <= 0) {
        c->timer += step;
        c->step = (c->step + 1) & 7;
    }
}

static void run_triangle(Channel *c, int cycles)
{
    c->timer -= cycles;
    int32_t step = c->period + 1;
    while (c->timer <= 0) {
        c->timer += step;
        // Ultrasonic periods (< 2) would only alias: hold the output.
        if (c->length && c->linear && c->period >= 2)
            c->step = (c->step + 1) & 31;
    }
}

static void run_noise(Channel *c, int cycles)
{
    c->timer -= cycles;
    while (c->timer <= 0) {
        c->timer += c->period;
        int fb = (c->lfsr & 1) ^ ((c->lfsr >> (c->mode ? 6 : 1)) & 1);
        c->lfsr = (uint16_t)((c->lfsr >> 1) | fb << 14);
    }
}

static int pulse_level(const Channel *c, int idx)
{
    if (!c->length || pulse_muted(c, idx) || !duty_table[c->duty][c->step])
        return 0;
    return env_out(&c->env);
}

void vapuRender(int16_t *out, int samples)
{
    for (int s = 0; s < samples; s++) {
        apu.cycle_frac += apu.cycles_per_sample_fp;
        int cycles = (int)(apu.cycle_frac >> 16);
        apu.cycle_frac &= 0xFFFF;

        frame_counter(cycles);
        run_pulse(&apu.ch[0], cycles);
        run_pulse(&apu.ch[1], cycles);
        run_triangle(&apu.ch[2], cycles);
        run_noise(&apu.ch[3], cycles);

        const Channel *t = &apu.ch[2], *n = &apu.ch[3];
        int p = pulse_level(&apu.ch[0], 0) + pulse_level(&apu.ch[1], 1);
        int tri = t->step < 16 ? 15 - t->step : t->step - 16;
        int noi = (n->length && !(n->lfsr & 1)) ? env_out(&n->env) : 0;
        int32_t x = apu.pulse_mix[p] + apu.tnd_mix[3 * tri + 2 * noi];

        // ~90 Hz high-pass (removes the DC offset of the DAC curves) and a
        // light low-pass against aliasing, as on the console.
        int32_t hp = x - apu.hp_prev_in + apu.hp_prev_out * 251 / 256;
        apu.hp_prev_in = x;
        apu.hp_prev_out = hp;
        apu.lp += (hp - apu.lp) / 2; // division rounds to zero: no DC left
        int32_t y = apu.lp;
        out[s] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    }
}
