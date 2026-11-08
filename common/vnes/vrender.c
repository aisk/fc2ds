#include "vrender.h"

// Same placeholder palette as common/nds/src/nes_palette.c.
static const uint32_t nes_rgb[64] = {
    0x7C7C7C, 0x0000FC, 0x0000BC, 0x4428BC, 0x940084, 0xA80020, 0xA81000, 0x881400,
    0x503000, 0x007800, 0x006800, 0x005800, 0x004058, 0x000000, 0x000000, 0x000000,
    0xBCBCBC, 0x0078F8, 0x0058F8, 0x6844FC, 0xD800CC, 0xE40058, 0xF83800, 0xE45C10,
    0xAC7C00, 0x00B800, 0x00A800, 0x00A844, 0x008888, 0x000000, 0x000000, 0x000000,
    0xF8F8F8, 0x3CBCFC, 0x6888FC, 0x9878F8, 0xF878F8, 0xF85898, 0xF87858, 0xFCA044,
    0xF8B800, 0xB8F818, 0x58D854, 0x58F898, 0x00E8D8, 0x787878, 0x000000, 0x000000,
    0xFCFCFC, 0xA4E4FC, 0xB8B8F8, 0xD8B8F8, 0xF8B8F8, 0xF8A4C0, 0xF0D0B0, 0xFCE0A8,
    0xF8D878, 0xD8F878, 0xB8F8B8, 0xB8F8D8, 0x00FCFC, 0xF8D8F8, 0x000000, 0x000000,
};

uint32_t vrenderRgb(uint8_t index)
{
    return nes_rgb[index & 0x3F];
}

// Colour 0-3 of pixel (px, py) of the tile at PPU pattern address `addr`.
static int pattern_pixel(const VrasterLine *l, uint16_t addr, int px, int py)
{
    int off = (l->chr[(addr >> 10) & 7] * 0x400 + (addr & 0x3FF) + py) % vnes->chr_size;
    const uint8_t *p = vnes->chr + off;
    int bit = 7 - px;
    return ((p[0] >> bit) & 1) | (((p[8] >> bit) & 1) << 1);
}

void vrenderFrame(const VrasterFrame *f, uint8_t out[240][256])
{
    for (int y = 0; y < 240; y++) {
        const VrasterLine *l = &f->line[y];
        uint8_t bg[256] = { 0 };
        if (l->mask & 0x08) {
            int py = l->y & 511;
            int row = py >> 3;
            for (int x = 0; x < 256; x++) {
                if (x < 8 && !(l->mask & 0x02))
                    continue;
                int px = (l->x + x) & 511;
                int col = px >> 3;
                uint8_t tile = vrasterNametable(l, col, row);
                int nrow = row & 31, ncol = col & 31;
                // Attribute byte ($3C0 + row/4 * 8 + col/4) of the same nametable.
                uint16_t aaddr = 0x3C0 + (nrow >> 2) * 8 + (ncol >> 2);
                uint8_t a = vrasterNametable(l, (col & 32) | (aaddr & 31), (row & 32) | (aaddr >> 5));
                int shift = ((nrow & 2) << 1) | (ncol & 2);
                int pal = (a >> shift) & 3;
                uint16_t base = (l->ctrl & 0x10) ? 0x1000 : 0;
                int c = pattern_pixel(l, base + tile * 16, px & 7, py & 7);
                bg[x] = c ? (uint8_t)(pal * 4 + c) : 0;
            }
        }
        for (int x = 0; x < 256; x++)
            out[y][x] = vnes->palette[bg[x]] & 0x3F;

        if (!(l->mask & 0x10))
            continue;
        int tall = l->ctrl & 0x20;
        int h = tall ? 16 : 8;
        // Lower OAM index wins: draw from the back.
        for (int i = 63; i >= 0; i--) {
            const uint8_t *s = &vnes->oam[i * 4];
            int sy = s[0] + 1;
            int r = y - sy;
            if (r < 0 || r >= h)
                continue;
            uint8_t tile = s[1], at = s[2];
            if (at & 0x80)
                r = h - 1 - r;
            uint16_t addr;
            if (tall)
                addr = ((tile & 1) ? 0x1000 : 0) + (tile & 0xFE) * 16 + (r >= 8 ? 16 : 0);
            else
                addr = ((l->ctrl & 0x08) ? 0x1000 : 0) + tile * 16;
            for (int px = 0; px < 8; px++) {
                int x = s[3] + px;
                if (x > 255 || (x < 8 && !(l->mask & 0x04)))
                    continue;
                int c = pattern_pixel(l, addr, (at & 0x40) ? 7 - px : px, r & 7);
                if (!c)
                    continue;
                if ((at & 0x20) && bg[x])
                    continue; // behind opaque background
                out[y][x] = vnes->palette[16 + (at & 3) * 4 + c] & 0x3F;
            }
        }
    }
}
