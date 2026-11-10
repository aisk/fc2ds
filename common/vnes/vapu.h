// 2A03 APU synthesizer: pulse 1/2, triangle and noise with envelopes,
// sweep, length and linear counters and the frame counter, mixed with the
// NES's non-linear DAC curves, and the DMC: the level written to $4011 and
// DPCM sample playback (read through vapuSetMemory). The DMC IRQ is not
// modelled. Platform neutral C, integer only after vapuInit.
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
// CPU bus reads for the DMC's samples ($8000-$FFFF), e.g. vnesRead. Without
// it, samples play as silence.
void vapuSetMemory(uint8_t (*read)(uint16_t addr));
// Play every sample byte with its bits in the opposite order (an
// enhancement for games whose samples were encoded with the bits
// reversed, a common mistake of the time). Off by default.
void vapuSetDmcReversed(int on);
