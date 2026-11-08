// Virtual NES system state without a CPU: RAM, MMC3, PPU memory and
// registers, joypad. Ported game code reads and writes the NES through this,
// so it runs unchanged on the host reference runner (common/tools/nesref) and
// on the NDS, where a renderer turns the PPU state into NDS graphics.
//
// Platform neutral C, no libnds or stdio.
#pragma once

#include <stdint.h>

// Registers that decide what each scanline shows, and a log of the writes
// to them during the visible lines, so a renderer can redo raster effects
// (mid-frame scroll and CHR changes); see vraster.h.
typedef struct {
    uint16_t t, v;
    uint8_t x, w; // fine X scroll, $2005/$2006 write toggle
    uint8_t ctrl, mask;
    uint8_t bank_select, regs[8], mirroring;
} VnesRaster;

typedef struct {
    uint8_t line;   // scanline (0-239) when it happened
    uint8_t value;
    uint16_t addr;  // $2000/$2001/$2005/$2006 write, $2002 read, $8000/$8001/$A000 write
} VnesRasterEvent;

#define VNES_RASTER_EVENTS 512

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
    uint8_t x, w, read_buffer;
    uint8_t nmi;      // NMI edge for the CPU (host runner only)

    // APU: last value written to $4000-$4017, and a hash of every write in
    // order (address and value), so the sound code can be verified.
    uint8_t apu[0x18];
    uint32_t apu_hash;

    // Joypad 1: bit 0 = A ... bit 7 = Right, in $4016 read order.
    uint8_t pad, pad_shift, strobe;

    // Raster state at the start of the current frame (pre-render line) and
    // the events of its visible lines.
    VnesRaster raster_start;
    VnesRasterEvent raster[VNES_RASTER_EVENTS];
    int raster_count;

    // Timing: PPU dots in the current scanline.
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

// Copy the raster registers as they are now.
void vnesRasterSave(VnesRaster *r);

// Advance PPU timing by CPU cycles (sets nmi, irq_pending; takes the raster
// snapshot on the pre-render line).
void vnesAddCycles(int cycles);

// Provided by the platform that runs the ported game (the host runner or the
// NDS main loop), called by ported code:
// - portWaitNmi: where the original waits for something only the NMI changes
//   (vblank): handle the rest of the frame (IRQ splits) and the NMI.
// - portTickEnd: where the original main loop reaches the end of a tick.
void portWaitNmi(void);
void portTickEnd(void);
