// NDS backend for virtual NES frames: shows ndsframe screens on the main
// (top) and sub (bottom) engines. BG0 of each engine is the NES nametable
// plane with per-line scroll by HBlank DMA; NES sprites become 8x8 OBJs.
#pragma once

#include <stdint.h>

#include "ndsframe.h"

// Set up both engines; converts the NES CHR to 4bpp once.
void ndsVideoInit(const uint8_t *chr, int chr_size);

// Build both screens from the frame that just finished (CPU work, any time
// before the next vblank).
void ndsVideoBuild(const VrasterFrame *f, const NdsLayout *layout);

// Copy the built screens to the hardware. Call right after the vblank
// starts (swiWaitForVBlank).
void ndsVideoCommit(void);
