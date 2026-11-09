// Calls between ported routines through their hooks, on the NDS.
//
// A port can keep calling routines by their original address with
// call6502(addr) and pass registers in a Cpu (portCpu()), the way it does
// while it is being ported. The host runner's --native mode dispatches such
// calls to the PORT_HOOK wrappers without running any 6502 code; this file
// does the same on the NDS, so the wrappers that `make verify` checked are
// the calling convention the game runs with. A game opts in by compiling
// port/ with PORT_REGS (see the game's Makefile); games that turned every
// call into a direct C call do not use it.
#include <stdlib.h>

#include "nesref.h"
#include "vnes.h"

extern const PortHook *const __start_port_hooks[] __attribute__((weak));
extern const PortHook *const __stop_port_hooks[] __attribute__((weak));

static Cpu cpu = { .s = 0xFF };
static const PortHook **sorted;
static int count;

Cpu *portCpu(void)
{
    return &cpu;
}

static int by_addr(const void *a, const void *b)
{
    return (*(const PortHook *const *)a)->addr - (*(const PortHook *const *)b)->addr;
}

static void init(void)
{
    count = __start_port_hooks ? (int)(__stop_port_hooks - __start_port_hooks) : 0;
    sorted = malloc((count + 1) * sizeof *sorted);
    for (int i = 0; i < count; i++)
        sorted[i] = __start_port_hooks[i];
    qsort(sorted, count, sizeof *sorted, by_addr);
}

// The bank a hook needs at addr: banked games have hooks for several
// pages at the same address (PORT_HOOK's bank, 8 KiB units, -1 = any).
static int bank_matches(const PortHook *h, uint16_t addr)
{
    return h->bank < 0 || addr < 0x8000 || h->bank == vnesPrgBank((addr - 0x8000) >> 13);
}

// Run the hook of the routine at addr with the banks mapped now.
void call6502(uint16_t addr)
{
    if (!sorted)
        init();
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sorted[mid]->addr < addr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    // lo is the first hook at addr, if there is one.
    for (int i = lo; i < count && sorted[i]->addr == addr; i++) {
        if (bank_matches(sorted[i], addr)) {
            cpu.pc = addr;
            sorted[i]->fn(&cpu);
            return;
        }
    }
    // No hook: a routine the port did not cover. Stop here rather than
    // run on with wrong state.
    for (;;)
        ;
}
