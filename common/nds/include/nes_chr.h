// NES CHR (2bpp planar) to NDS tile (4bpp packed) conversion.
#ifndef FC2DS_NES_CHR_H
#define FC2DS_NES_CHR_H

#include <stdint.h>

#define NES_TILE_BYTES 16
#define NDS_TILE4_WORDS 8

// Convert `tiles` NES tiles at `src` into 4bpp NDS tiles at `dst`.
// Colour index n (0-3) of the NES tile becomes index n of the NDS tile, so the
// result can be used with any 16-colour palette slot. `dst` may point to VRAM
// (only 32-bit writes are used).
void nesChrTo4bpp(const uint8_t *src, uint32_t *dst, int tiles);

#endif
