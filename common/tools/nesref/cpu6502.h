// Minimal 6502 (2A03) core for the reference runner: official opcodes only,
// instruction-level timing with page-cross and branch penalties.
#pragma once

#include <stdint.h>

typedef struct Cpu {
    uint16_t pc;
    uint8_t a, x, y, s, p;
    uint64_t cycles;
    uint8_t (*read)(void *ctx, uint16_t addr);
    void (*write)(void *ctx, uint16_t addr, uint8_t v);
    void *ctx;
    int nmi;      // set by the PPU, taken before the next instruction
    int irq;      // level, taken when I is clear
} Cpu;

enum { FLAG_C = 1, FLAG_Z = 2, FLAG_I = 4, FLAG_D = 8, FLAG_B = 16, FLAG_U = 32, FLAG_V = 64, FLAG_N = 128 };

void cpuReset(Cpu *c);
// Run one instruction (or interrupt entry). Returns the cycles used, or -1
// for an opcode the core does not implement.
int cpuStep(Cpu *c);
