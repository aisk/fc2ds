// Virtual NES system state without a CPU: RAM, MMC3, PPU memory and
// registers, joypad. Ported game code reads and writes the NES through this,
// so it runs unchanged on the host reference runner (common/tools/nesref) and
// on the NDS, where a renderer turns the PPU state into NDS graphics.
//
// Platform neutral C, no libnds or stdio.
#pragma once

#include <stdint.h>

typedef struct Vnes {
    const uint8_t *prg, *chr;
    int prg_size, chr_size;
    uint8_t ram[0x800];
    uint8_t prg_ram[0x2000];

    // MMC3
    uint8_t bank_select, regs[8], mirroring;
    uint8_t irq_latch, irq_counter, irq_reload, irq_enabled, irq_pending;

    // PPU
    uint8_t ctrl, mask, status, oam_addr;
    uint8_t vram[0x1000], palette[32], oam[256];
    uint16_t v, t;
    uint8_t w, read_buffer;
    uint8_t nmi;      // NMI edge for the CPU (host runner only)

    // Joypad 1: bit 0 = A ... bit 7 = Right, in $4016 read order.
    uint8_t pad, pad_shift, strobe;

    // Timing (host runner): PPU dots in the current scanline.
    long dots, dots_per_line, frame;
    int line;
    int dma_cycles;
} Vnes;

// The instance that vnesRead/vnesWrite and nes_ram act on.
extern Vnes *vnes;
#define nes_ram (vnes->ram)

void vnesInit(Vnes *n, const uint8_t *prg, int prg_size, const uint8_t *chr, int chr_size);

// CPU bus.
uint8_t vnesRead(uint16_t addr);
void vnesWrite(uint16_t addr, uint8_t v);

// PPU address space ($0000-$3FFF) and helpers for a renderer.
uint8_t vnesPpuRead(uint16_t addr);
uint16_t vnesNametableIndex(uint16_t addr); // $2000-$2FFF -> vram index
int vnesPrgBank(int slot);                  // 8 KiB bank mapped at $8000 + slot * $2000
int vnesChrOffset(uint16_t addr);           // byte offset in CHR for a pattern address

// Host runner: advance PPU timing by CPU cycles (sets nmi, irq_pending).
void vnesAddCycles(int cycles);
