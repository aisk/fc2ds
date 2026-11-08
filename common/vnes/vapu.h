// 2A03 APU synthesizer: pulse 1/2, triangle and noise with envelopes,
// sweep, length and linear counters and the frame counter, mixed with the
// NES's non-linear DAC curves. DPCM is not implemented yet (no ported game
// uses it so far). Platform neutral C, integer only after vapuInit.
//
// The caller decides timing: vapuWrite applies a register write at once,
// vapuRender produces the next samples. Both nesref --wav and the NDS render
// once per frame, so writes take effect at frame granularity, which is how
// a once-per-frame sound driver sounds anyway.
#pragma once

#include <stdint.h>

#define VAPU_CPU_HZ 1789773

void vapuInit(int sample_rate);
void vapuWrite(uint16_t addr, uint8_t value); // $4000-$4017
void vapuRender(int16_t *out, int samples);
