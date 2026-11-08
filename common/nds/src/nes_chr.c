#include "nes_chr.h"

// Spread the 8 bits of a byte into the low bit of 8 nibbles, MSB (leftmost
// pixel) going to the lowest nibble, which is the leftmost pixel on NDS.
static uint32_t spread(uint8_t b)
{
    uint32_t r = 0;
    for (int x = 0; x < 8; x++)
        r |= (uint32_t)((b >> (7 - x)) & 1) << (x * 4);
    return r;
}

void nesChrTo4bpp(const uint8_t *src, uint32_t *dst, int tiles)
{
    static uint32_t table[256];
    static int ready;

    if (!ready) {
        for (int i = 0; i < 256; i++)
            table[i] = spread(i);
        ready = 1;
    }

    for (int t = 0; t < tiles; t++) {
        for (int y = 0; y < 8; y++)
            dst[y] = table[src[y]] | (table[src[y + 8]] << 1);
        src += NES_TILE_BYTES;
        dst += NDS_TILE4_WORDS;
    }
}
