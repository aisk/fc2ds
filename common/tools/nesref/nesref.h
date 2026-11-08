// Hooks: ported C routines that replace original 6502 routines in the
// reference runner's "port" machine. See nesref.c.
#pragma once

#include <stdint.h>

#include "cpu6502.h"

typedef struct {
    uint16_t addr; // entry point of the original routine
    int bank;      // 8 KiB PRG bank that must be mapped there, -1 = any
    // Runs instead of the routine; afterwards the runner returns with RTS.
    // It gets the CPU for routines that take or return values in registers.
    void (*fn)(Cpu *cpu);
    const char *name;
} PortHook;

// Provided by the game; weak so that nesref also builds without hooks.
extern const PortHook port_hooks[] __attribute__((weak));
extern const int port_hook_count __attribute__((weak));

// Run an original 6502 routine from C until it returns (port machine only).
// Registers are passed in and out through the Cpu.
Cpu *portCpu(void);
void call6502(uint16_t addr);
