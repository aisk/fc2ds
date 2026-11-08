// Per-scanline view of a virtual NES frame: replays the raster snapshot and
// event log that vnes records (mid-frame scroll, CHR bank and mirroring
// changes) into what each of the 240 visible lines shows. Renderers (the
// host PNG writer, the NDS renderer) draw from this instead of emulating
// the PPU dot by dot.
//
// Timing model: a register access logged during line L (an IRQ handler
// runs right after the MMC3 counter clocks at the end of line L-1) takes
// effect from line L + 1. The loopy v register is advanced like the PPU
// does: Y at the end of each line, X copied from t at the start of the next.
#pragma once

#include <stdint.h>

#include "vnes.h"

typedef struct {
    uint16_t x;       // 0-511: horizontal position of the line's first pixel
                      // in the 2x2 nametable plane (bit 8 = right nametable)
    uint16_t y;       // 0-511: row in the plane, 256 per nametable (rows
                      // 240-255 are the attribute table, as on the NES)
    uint8_t ctrl;     // PPUCTRL (pattern tables, sprite size)
    uint8_t mask;     // PPUMASK (bit 3 BG on, bit 4 sprites on)
    uint8_t mirroring;// MMC3: 0 vertical, 1 horizontal
    uint8_t chr[8];   // 1 KiB CHR bank at PPU $0000, $0400 ... $1C00
} VrasterLine;

typedef struct {
    VrasterLine line[240];
} VrasterFrame;

// Build the frame from vnes->raster_start and the events of the frame that
// just finished its visible lines.
void vrasterBuild(VrasterFrame *f);

// Nametable byte at tile column 0-63, row 0-63 of the plane (rows 30-31 of
// each nametable are its attribute table), with the line's mirroring.
uint8_t vrasterNametable(const VrasterLine *l, int col, int row);
