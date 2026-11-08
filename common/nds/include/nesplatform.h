// Runs a ported NES game on the NDS: the virtual NES in common/vnes keeps
// the original frame timing (scanline counter for MMC3 IRQs, vblank NMI),
// the game's C main loop calls portWaitNmi() where the original waits for
// vblank, and every finished frame is shown with ndsvideo.
#pragma once

#include <stdint.h>

typedef struct {
    const uint8_t *prg, *chr;
    int prg_size, chr_size;
    void (*nmi)(void); // the game's NMI handler
    void (*irq)(void); // the game's IRQ handler (acknowledges the MMC3 IRQ)
} NesPlatform;

// Set up video and the virtual NES. Call before the game's main loop.
void nesPlatformInit(const NesPlatform *p);
