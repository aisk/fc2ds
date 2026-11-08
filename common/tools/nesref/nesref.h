// Hooks: ported C routines that replace original 6502 routines in the
// reference runner's "port" machine. See nesref.c.
#pragma once

#include <stdint.h>

#include "cpu6502.h"

typedef struct {
    uint16_t addr; // entry point of the original routine
    int bank;      // 8 KiB PRG bank that must be mapped there, -1 = any
    // Runs instead of the routine; afterwards the runner returns with RTS
    // (or RTI for interrupt handlers). It gets the CPU for routines that
    // take or return values in registers.
    void (*fn)(Cpu *cpu);
    const char *name;
    int rti;
} PortHook;

// Register a hook from any source file:
//     PORT_HOOK(0xD811, -1, h_SetPrgPage);
//     PORT_HOOK(0xF109, -1, h_Nmi, .rti = 1);
// The linker collects them in the port_hooks section.
#define PORT_HOOK(addr_, bank_, fn_, ...)                                                    \
    static const PortHook hook_##fn_ = { .addr = (addr_), .bank = (bank_), .fn = (fn_),     \
                                         .name = #fn_ + 2, __VA_ARGS__ };                  \
    static const PortHook *const hookp_##fn_ __attribute__((used, section("port_hooks"))) = \
        &hook_##fn_

// Helpers for hook wrappers.
static inline void cpuSetZN(Cpu *c, uint8_t v)
{
    c->p = (c->p & ~(FLAG_Z | FLAG_N)) | (v ? 0 : FLAG_Z) | (v & FLAG_N);
}

static inline void cpuSetC(Cpu *c, int carry)
{
    c->p = (c->p & ~FLAG_C) | (carry ? FLAG_C : 0);
}

// Run an original 6502 routine from C until it returns (port machine only).
// Registers are passed in and out through the Cpu.
Cpu *portCpu(void);
void call6502(uint16_t addr);
