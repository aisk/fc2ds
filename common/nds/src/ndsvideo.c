#include "ndsvideo.h"

#include <nds.h>
#include <stdlib.h>
#include <string.h>

#include "nes_chr.h"
#include "nes_palette.h"

// VRAM layout of each engine: BG0 map at map base 0 (4 blocks, 8 KiB),
// BG0 tiles at tile base 1 (16 KiB), 1024 tiles.
#define MAP_BASE 0
#define TILE_BASE 1

typedef struct {
    int sub;
    int bg;
    NdsScreen screen;
    NdsTileCache bg_cache, obj_cache;
    uint16_t scroll[192][2]; // HOFS, VOFS per line, read by HBlank DMA
    int has_frame;
} Engine;

static Engine engines[2];
static uint32_t *chr4;

static void engine_init(Engine *e, int sub)
{
    e->sub = sub;
    ndsTileCacheInit(&e->bg_cache);
    ndsTileCacheInit(&e->obj_cache);
    if (!sub)
        e->bg = bgInit(0, BgType_Text4bpp, BgSize_T_512x512, MAP_BASE, TILE_BASE);
    else
        e->bg = bgInitSub(0, BgType_Text4bpp, BgSize_T_512x512, MAP_BASE, TILE_BASE);
    bgSetPriority(e->bg, 1);
    volatile uint32_t *map = (uint32_t *)bgGetMapPtr(e->bg);
    for (int i = 0; i < 64 * 64 / 2; i++)
        map[i] = 0;
}

void ndsVideoInit(const uint8_t *chr, int chr_size)
{
    chr4 = malloc((size_t)chr_size * 2);
    nesChrTo4bpp(chr, chr4, chr_size / 16);

    videoSetMode(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D | DISPLAY_WIN0_ON);
    videoSetModeSub(MODE_0_2D | DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE | DISPLAY_SPR_1D | DISPLAY_WIN0_ON);
    vramSetPrimaryBanks(VRAM_A_MAIN_BG, VRAM_B_MAIN_SPRITE, VRAM_C_SUB_BG, VRAM_D_SUB_SPRITE);
    engine_init(&engines[0], 0);
    engine_init(&engines[1], 1);
    for (int i = 0; i < 128; i++) {
        OAM[i * 4] = ATTR0_DISABLED;
        OAM_SUB[i * 4] = ATTR0_DISABLED;
    }
    // Window 0 = the lines that show the frame; nothing outside it.
    WIN_OUT = 0;
    SUB_WIN_OUT = 0;
    WIN_IN = 0x1F;     // BG0-3, OBJ inside window 0
    SUB_WIN_IN = 0x1F;
}

void ndsVideoBuild(const VrasterFrame *f, const NdsLayout *lay)
{
    Engine *top = &engines[0], *bot = &engines[1];
    ndsScreenBuild(&top->screen, &top->bg_cache, &top->obj_cache, f, lay->top_first, lay->top_end, 0, 1);
    top->has_frame = 1;
    if (lay->bot_end > lay->bot_first) {
        ndsScreenBuild(&bot->screen, &bot->bg_cache, &bot->obj_cache, f, lay->bot_first, lay->bot_end,
                       lay->bot_dst, 0);
        bot->has_frame = 1;
    } else {
        bot->has_frame = 0;
    }
}

// VRAM takes no 8-bit writes: copy whole words.
static void copy_words(volatile uint32_t *dst, const uint32_t *src, int words)
{
    for (int i = 0; i < words; i++)
        dst[i] = src[i];
}

static void upload_tiles(NdsTileCache *c, uint32_t *vram)
{
    for (int i = 0; i < c->dirty_count; i++) {
        uint16_t s = c->dirty[i];
        if (c->key[s] != 0xFFFF)
            copy_words(vram + s * 8, ndsChrTile(chr4, c->key[s]), 8);
    }
    c->dirty_count = 0;
}

static void set_palette(uint16_t *bg, uint16_t *obj, const NdsScreen *s)
{
    bg[0] = nesColor(s->backdrop);
    for (int p = 0; p < 4; p++)
        for (int c = 1; c < 4; c++) {
            bg[p * 16 + c] = nesColor(s->bg_pal[p][c]);
            obj[p * 16 + c] = nesColor(s->obj_pal[p][c]);
        }
}

static void commit(Engine *e)
{
    volatile uint16_t *oam = e->sub ? OAM_SUB : OAM;
    uint16_t *bgpal = e->sub ? BG_PALETTE_SUB : BG_PALETTE;
    uint16_t *objpal = e->sub ? SPRITE_PALETTE_SUB : SPRITE_PALETTE;
    int dma = e->sub ? 1 : 0;
    volatile uint16_t *scroll_reg = e->sub ? &REG_BG0HOFS_SUB : &REG_BG0HOFS;

    DMA_CR(dma) = 0;
    if (!e->has_frame) {
        // Nothing on this screen: backdrop only.
        if (e->sub) {
            SUB_WIN0_Y0 = 0;
            SUB_WIN0_Y1 = 0;
        } else {
            WIN0_Y0 = 0;
            WIN0_Y1 = 0;
        }
        bgpal[0] = 0;
        return;
    }
    NdsScreen *s = &e->screen;
    upload_tiles(&e->bg_cache, (uint32_t *)bgGetGfxPtr(e->bg));
    upload_tiles(&e->obj_cache, e->sub ? (uint32_t *)SPRITE_GFX_SUB : (uint32_t *)SPRITE_GFX);
    copy_words((uint32_t *)bgGetMapPtr(e->bg), (const uint32_t *)s->map, sizeof s->map / 4);
    set_palette(bgpal, objpal, s);

    for (int i = 0; i < 128; i++) {
        if (i < s->oam_count) {
            oam[i * 4] = s->oam[i].attr0;
            oam[i * 4 + 1] = s->oam[i].attr1;
            oam[i * 4 + 2] = s->oam[i].attr2;
        } else {
            oam[i * 4] = ATTR0_DISABLED;
        }
    }

    if (e->sub) {
        SUB_WIN0_X0 = 0;
        SUB_WIN0_X1 = 255;
        SUB_WIN0_Y0 = s->first;
        SUB_WIN0_Y1 = s->end;
        SUB_WIN_IN = (s->bg_on ? 0x01 : 0) | (s->obj_on ? 0x10 : 0);
    } else {
        WIN0_X0 = 0;
        WIN0_X1 = 255;
        WIN0_Y0 = s->first;
        WIN0_Y1 = s->end;
        WIN_IN = (s->bg_on ? 0x01 : 0) | (s->obj_on ? 0x10 : 0);
    }

    // Line 0 now, lines 1-191 by HBlank DMA (it runs at the end of each
    // line and sets up the next one).
    for (int i = 0; i < 192; i++) {
        e->scroll[i][0] = s->hofs[i];
        e->scroll[i][1] = s->vofs[i];
    }
    DC_FlushRange(e->scroll, sizeof e->scroll);
    scroll_reg[0] = e->scroll[0][0];
    scroll_reg[1] = e->scroll[0][1];
    DMA_SRC(dma) = (uint32_t)&e->scroll[1][0];
    DMA_DEST(dma) = (uint32_t)scroll_reg;
    DMA_CR(dma) = DMA_ENABLE | DMA_REPEAT | DMA_START_HBL | DMA_16_BIT | DMA_SRC_INC | DMA_DST_RESET | 2;
}

void ndsVideoCommit(void)
{
    commit(&engines[0]);
    commit(&engines[1]);
}
