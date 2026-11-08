// NES master palette mapped to NDS RGB15.
#ifndef FC2DS_NES_PALETTE_H
#define FC2DS_NES_PALETTE_H

#include <stdint.h>

// RGB15 colour for NES palette index 0x00-0x3F (higher bits are ignored).
uint16_t nesColor(uint8_t index);

#endif
