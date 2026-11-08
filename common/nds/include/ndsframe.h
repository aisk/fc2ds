// Maps a virtual NES frame (common/vnes/vraster.h) onto NDS 2D hardware
// state for one screen, without touching the hardware: a 512x512 text BG
// (the NES 2x2 nametable plane), per-line BG scroll for raster effects,
// 8x8 sprites, palettes, and the CHR tiles they need. The NDS backend
// (ndsvideo.c) copies the result to VRAM and registers; the host runner can
// compose the same state into a picture to check it without an emulator.
//
// Platform neutral C, no libnds.
#pragma once

#include <stdint.h>

#include "vraster.h"

#define NDS_TILE_SLOTS 1024  // 4bpp tiles a BG / OBJ layer can index
#define NES_CHR_TILES 8192   // 128 KiB of NES CHR

// Which NDS tile slot holds which NES CHR tile. A NES tile is identified
// by its 16-byte index in CHR ROM (1 KiB bank * 64 + tile in bank).
typedef struct {
    uint16_t slot_of[NES_CHR_TILES]; // 0xFFFF: not loaded
    uint16_t key[NDS_TILE_SLOTS];    // 0xFFFF: free
    uint32_t used[NDS_TILE_SLOTS];   // frame stamp of last use
    uint32_t stamp;
    uint16_t hand;
    // Slots whose tile changed since the backend last uploaded them.
    uint16_t dirty[NDS_TILE_SLOTS];
    int dirty_count;
} NdsTileCache;

void ndsTileCacheInit(NdsTileCache *c);

typedef struct {
    uint16_t attr0, attr1, attr2;
} NdsOamEntry;

typedef struct {
    // Text BG, 64x64 entries in NDS screen block order (4 blocks of 32x32:
    // top-left, top-right, bottom-left, bottom-right).
    uint16_t map[64 * 64];
    uint16_t hofs[192], vofs[192];
    // Lines first..end-1 show the frame; others show only the backdrop
    // (the backend uses a window).
    int first, end;
    int bg_on, obj_on;
    // PPUMASK left column: BG / sprites shown in x 0-7 (games hide it to
    // cover the column being redrawn while scrolling).
    int left_bg, left_obj;
    // Pixels from this x on show only the backdrop (256 = none), to hide
    // what the NES leaves at the right edge while scrolling.
    int right;
    // NES colour indices: backdrop, BG palettes 0-3, sprite palettes 0-3
    // (colour 0 of each palette is transparent).
    uint8_t backdrop;
    uint8_t bg_pal[4][4], obj_pal[4][4];
    NdsOamEntry oam[128];
    int oam_count;
} NdsScreen;

// Build a screen from NES lines [nes_first, nes_end) of the frame, drawn
// from screen line `dst` down. Sprites are included when `sprites` is set.
// Uses the current vnes for nametables, palette RAM and OAM.
void ndsScreenBuild(NdsScreen *s, NdsTileCache *bg, NdsTileCache *obj, const VrasterFrame *f,
                    int nes_first, int nes_end, int dst, int sprites);

// Which NES lines go where (decided per frame by the game).
typedef struct {
    int top_first, top_end;          // NES lines on the top screen, from line 0
    int bot_first, bot_end, bot_dst; // NES lines on the bottom screen, from line bot_dst
    int top_crop_right;              // top screen: hide this many pixels at the right edge
} NdsLayout;

// Provided by the game.
void GameScreenLayout(const VrasterFrame *f, NdsLayout *out);

// NES CHR tile `key` as an NDS 4bpp tile (8 words), from CHR converted once
// with nesChrTo4bpp.
static inline const uint32_t *ndsChrTile(const uint32_t *chr4, uint16_t key)
{
    return chr4 + key * 8;
}

// Host check: draw a built screen as 256x192 NES colour indices.
void ndsScreenCompose(const NdsScreen *s, const NdsTileCache *bg, const NdsTileCache *obj,
                      const uint32_t *chr4, uint8_t out[192][256]);
