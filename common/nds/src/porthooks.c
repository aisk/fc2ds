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

// Run the hook of the routine at addr. NROM and fixed-bank games have one
// hook per address; the bank of a hook is not checked.
void call6502(uint16_t addr)
{
    if (!sorted)
        init();
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sorted[mid]->addr == addr) {
            cpu.pc = addr;
            sorted[mid]->fn(&cpu);
            return;
        }
        if (sorted[mid]->addr < addr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    // No hook: a routine the port did not cover. Stop here rather than
    // run on with wrong state.
    for (;;)
        ;
}
