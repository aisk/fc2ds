#include "ndsframe.h"

#include <string.h>

// Built every frame for both screens: on the NDS run it from ITCM as ARM
// code, away from main RAM wait states.
#if defined(ARM9)
#include <nds/ndstypes.h>
#define NDSFRAME_HOT ITCM_CODE ARM_CODE
#else
#define NDSFRAME_HOT
#endif

void ndsTileCacheInit(NdsTileCache *c)
{
    memset(c->slot_of, 0xFF, sizeof c->slot_of);
    memset(c->key, 0xFF, sizeof c->key);
    memset(c->used, 0, sizeof c->used);
    c->stamp = 1;
    c->hand = 0;
    c->dirty_count = 0;
}

// Slot for NES CHR tile `key`, loading it into a slot not used in this
// frame if needed.
static inline uint16_t cache_get(NdsTileCache *c, uint16_t key)
{
    uint16_t s = c->slot_of[key];
    if (s != 0xFFFF) {
        c->used[s] = c->stamp;
        return s;
    }
    for (int n = 0; n < NDS_TILE_SLOTS; n++) {
        s = c->hand;
        c->hand = (c->hand + 1) % NDS_TILE_SLOTS;
        if (c->used[s] != c->stamp)
            break;
    }
    if (c->key[s] != 0xFFFF)
        c->slot_of[c->key[s]] = 0xFFFF;
    c->key[s] = key;
    c->slot_of[key] = s;
    c->used[s] = c->stamp;
    if (c->dirty_count < NDS_TILE_SLOTS)
        c->dirty[c->dirty_count++] = s;
    return s;
}

// NES CHR tile at PPU pattern address `addr` with the line's banks.
static uint16_t chr_key(const VrasterLine *l, uint16_t addr)
{
    int bank = l->chr[(addr >> 10) & 7] % (NES_CHR_TILES / 64);
    return (uint16_t)(bank * 64 + ((addr >> 4) & 63));
}

static int map_index(int col, int row)
{
    return ((row >> 5) * 2 + (col >> 5)) * 1024 + (row & 31) * 32 + (col & 31);
}

// vrasterNametable, inlined for the cell loop.
static inline uint8_t nametable(const VrasterLine *l, int col, int row)
{
    int a = (row & 32) << 6 | (col & 32) << 5 | (row & 31) << 5 | (col & 31);
    if (l->mirroring) // horizontal
        return vnes->vram[((a >> 1) & 0x400) | (a & 0x3FF)];
    return vnes->vram[a & 0x7FF];
}

// Fill the map cell (col, row) of the plane as the line shows it.
static inline void build_cell(NdsScreen *s, NdsTileCache *bg, const VrasterLine *l, int col, int row)
{
    uint8_t tile = nametable(l, col, row);
    int nrow = row & 31, ncol = col & 31;
    uint16_t aaddr = 0x3C0 + (nrow >> 2) * 8 + (ncol >> 2);
    uint8_t a = nametable(l, (col & 32) | (aaddr & 31), (row & 32) | (aaddr >> 5));
    int pal = (a >> (((nrow & 2) << 1) | (ncol & 2))) & 3;
    uint16_t base = (l->ctrl & 0x10) ? 0x1000 : 0;
    uint16_t slot = cache_get(bg, chr_key(l, base + tile * 16));
    s->map[map_index(col, row)] = slot | pal << 12;
}

static void add_sprite(NdsScreen *s, NdsTileCache *obj, const VrasterLine *l, uint16_t addr, int x, int y,
                       uint8_t at)
{
    if (s->oam_count >= 128)
        return;
    uint16_t slot = cache_get(obj, chr_key(l, addr));
    NdsOamEntry *e = &s->oam[s->oam_count++];
    e->attr0 = (uint16_t)(y & 0xFF);                          // square, 16 colours
    e->attr1 = (uint16_t)((x & 0x1FF) | (at & 0x40 ? 1 << 12 : 0) | (at & 0x80 ? 1 << 13 : 0)); // 8x8
    e->attr2 = (uint16_t)(slot | (at & 0x20 ? 2 : 0) << 10 | (at & 3) << 12);
}

NDSFRAME_HOT void ndsScreenBuild(NdsScreen *s, NdsTileCache *bg, NdsTileCache *obj, const VrasterFrame *f,
                                 int nes_first, int nes_end, int dst, int sprites)
{
    bg->stamp++;
    obj->stamp++;
    if (nes_end > 240)
        nes_end = 240;
    int n = nes_end - nes_first;
    if (dst + n > 192)
        n = 192 - dst;
    s->first = dst;
    s->end = dst + n;

    const VrasterLine *top = &f->line[nes_first];
    s->bg_on = (top->mask & 0x08) != 0;
    s->obj_on = sprites && (top->mask & 0x10);
    s->left_bg = (top->mask & 0x02) != 0;
    s->left_obj = (top->mask & 0x04) != 0;

    // Per-line scroll, and the map cells the lines show. A cell is built
    // with the first line that shows it (its CHR banks and mirroring); lines
    // of the same row may show different columns (e.g. switching nametables
    // line by line), so this is tracked per cell, not per row.
    static uint64_t cell_done[64];
    memset(cell_done, 0, sizeof cell_done);
    for (int i = 0; i < 192; i++) {
        int line = i - dst + nes_first;
        if (i < s->first || i >= s->end)
            line = nes_first;
        const VrasterLine *l = &f->line[line];
        s->hofs[i] = l->x & 511;
        s->vofs[i] = (uint16_t)((l->y - i) & 511);
        if (i < s->first || i >= s->end || !s->bg_on)
            continue;
        int row = (l->y >> 3) & 63;
        int col0 = (l->x >> 3) & 63;
        // The 33 columns the line shows, as a mask: most lines only show
        // cells built by the line above.
        uint64_t want = 0x1FFFFFFFFull << col0 | (col0 > 31 ? 0x1FFFFFFFFull >> (64 - col0) : 0);
        if ((cell_done[row] & want) == want)
            continue;
        for (int c = 0; c <= 32; c++) {
            int col = (col0 + c) & 63;
            if (cell_done[row] >> col & 1)
                continue;
            cell_done[row] |= 1ull << col;
            build_cell(s, bg, l, col, row);
        }
    }

    // Palettes. NES palette RAM: $3F00 backdrop, BG palettes at $3F01..,
    // sprite palettes at $3F11...
    s->backdrop = vnes->palette[0] & 0x3F;
    for (int p = 0; p < 4; p++)
        for (int c = 0; c < 4; c++) {
            s->bg_pal[p][c] = vnes->palette[p * 4 + c] & 0x3F;
            s->obj_pal[p][c] = vnes->palette[16 + p * 4 + c] & 0x3F;
        }

    // Sprites: NES OAM order is kept (lower index in front), 8x16 sprites
    // become two 8x8 ones.
    s->oam_count = 0;
    if (!s->obj_on)
        return;
    for (int i = 0; i < 64; i++) {
        const uint8_t *o = &vnes->oam[i * 4];
        int y = o[0] + 1;
        if (y >= 240)
            continue;
        const VrasterLine *l = &f->line[y < nes_first ? nes_first : y >= nes_end ? nes_end - 1 : y];
        int sy = y - nes_first + dst;
        if (sy >= s->end || sy + 16 <= s->first)
            continue;
        uint8_t tile = o[1], at = o[2];
        if (l->ctrl & 0x20) {
            uint16_t base = (tile & 1) ? 0x1000 : 0;
            uint16_t t0 = base + (tile & 0xFE) * 16, t1 = t0 + 16;
            if (at & 0x80) {
                uint16_t tmp = t0;
                t0 = t1;
                t1 = tmp;
            }
            add_sprite(s, obj, l, t0, o[3], sy, at);
            add_sprite(s, obj, l, t1, o[3], sy + 8, at);
        } else {
            add_sprite(s, obj, l, ((l->ctrl & 0x08) ? 0x1000 : 0) + tile * 16, o[3], sy, at);
        }
    }
}

// --- Host check -----------------------------------------------------------

static int tile_pixel(const uint32_t *chr4, uint16_t key, int x, int y)
{
    return (ndsChrTile(chr4, key)[y] >> (x * 4)) & 15;
}

void ndsScreenCompose(const NdsScreen *s, const NdsTileCache *bg, const NdsTileCache *obj,
                      const uint32_t *chr4, uint8_t out[192][256])
{
    for (int y = 0; y < 192; y++) {
        uint8_t bgc[256] = { 0 }; // colour index within palette, 0 = transparent
        for (int x = 0; x < 256; x++)
            out[y][x] = s->backdrop;
        if (y < s->first || y >= s->end)
            continue;
        if (s->bg_on) {
            int py = (y + s->vofs[y]) & 511;
            for (int x = s->left_bg ? 0 : 8; x < 256; x++) {
                int px = (x + s->hofs[y]) & 511;
                uint16_t e = s->map[map_index(px >> 3, py >> 3)];
                uint16_t key = bg->key[e & 0x3FF];
                if (key == 0xFFFF)
                    continue;
                int c = tile_pixel(chr4, key, px & 7, py & 7);
                bgc[x] = (uint8_t)c;
                if (c)
                    out[y][x] = s->bg_pal[e >> 12][c];
            }
        }
        // Sprites: lowest OAM index wins among the same priority; priority
        // 2 sprites go behind opaque BG pixels.
        for (int i = s->oam_count - 1; i >= 0; i--) {
            const NdsOamEntry *e = &s->oam[i];
            int sy = e->attr0 & 0xFF;
            if (sy >= 192)
                sy -= 256;
            int r = y - sy;
            if (r < 0 || r >= 8)
                continue;
            int sx = e->attr1 & 0x1FF;
            if (sx >= 256)
                sx -= 512;
            uint16_t key = obj->key[e->attr2 & 0x3FF];
            if (key == 0xFFFF)
                continue;
            int behind = ((e->attr2 >> 10) & 3) == 2;
            for (int px = 0; px < 8; px++) {
                int x = sx + px;
                if (x < (s->left_obj ? 0 : 8) || x > 255)
                    continue;
                int tx = (e->attr1 & (1 << 12)) ? 7 - px : px;
                int ty = (e->attr1 & (1 << 13)) ? 7 - r : r;
                int c = tile_pixel(chr4, key, tx, ty);
                if (!c || (behind && bgc[x]))
                    continue;
                out[y][x] = s->obj_pal[(e->attr2 >> 12) & 3][c];
            }
        }
    }
}
