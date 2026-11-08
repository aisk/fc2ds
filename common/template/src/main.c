// Skeleton: CHR viewer on the top screen, text console on the bottom screen.
#include <nds.h>
#include <stdio.h>

#include "nes_chr.h"

extern const uint8_t @NAME@_chr[], @NAME@_chr_end[];

#define COLS 32
#define ROWS 24
#define VIEW_TILES (COLS * ROWS)

static const u16 palettes[][4] = {
    { RGB15(0, 0, 0), RGB15(10, 10, 10), RGB15(21, 21, 21), RGB15(31, 31, 31) },
    { RGB15(0, 0, 0), RGB15(31, 8, 4), RGB15(31, 24, 10), RGB15(31, 31, 31) },
    { RGB15(4, 8, 16), RGB15(4, 20, 4), RGB15(20, 28, 8), RGB15(31, 31, 31) },
};
#define NUM_PALETTES (int)(sizeof(palettes) / sizeof(palettes[0]))

static u32 *tile_gfx;

static void show_tiles(int first)
{
    nesChrTo4bpp(@NAME@_chr + first * NES_TILE_BYTES, tile_gfx, VIEW_TILES);
}

static void set_palette(int p)
{
    for (int i = 0; i < 4; i++)
        BG_PALETTE[i] = palettes[p][i];
}

static void print_status(int first, int total, int pal)
{
    consoleClear();
    printf("@TITLE@ - fc2ds\n");
    printf("CHR viewer\n\n");
    printf("tiles   %04X-%04X / %04X\n", first, first + VIEW_TILES - 1, total);
    printf("CHR     $%05X-$%05X\n", first * NES_TILE_BYTES,
           (first + VIEW_TILES) * NES_TILE_BYTES - 1);
    printf("1K bank %02X-%02X\n", first / 64, (first + VIEW_TILES - 1) / 64);
    printf("palette %d\n\n", pal);
    printf("Up/Down  scroll 1 row\n");
    printf("L/R      scroll 1 page\n");
    printf("A        next palette\n");
}

int main(void)
{
    int total = (@NAME@_chr_end - @NAME@_chr) / NES_TILE_BYTES;
    int max_first = total - VIEW_TILES;
    int first = 0, pal = 0;

    videoSetMode(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bg = bgInit(0, BgType_Text4bpp, BgSize_T_256x256, 0, 1);
    tile_gfx = (u32 *)bgGetGfxPtr(bg);

    u16 *map = bgGetMapPtr(bg);
    for (int i = 0; i < 32 * 32; i++)
        map[i] = i < VIEW_TILES ? i : 0;

    consoleDemoInit();

    set_palette(pal);
    show_tiles(first);
    print_status(first, total, pal);

    keysSetRepeat(15, 4);

    while (1) {
        swiWaitForVBlank();
        scanKeys();
        int down = keysDownRepeat();
        int old_first = first;

        if (down & KEY_DOWN)
            first += COLS;
        if (down & KEY_UP)
            first -= COLS;
        if (down & KEY_R)
            first += VIEW_TILES;
        if (down & KEY_L)
            first -= VIEW_TILES;
        if (first < 0)
            first = 0;
        if (first > max_first)
            first = max_first;

        if (keysDown() & KEY_A) {
            pal = (pal + 1) % NUM_PALETTES;
            set_palette(pal);
        }

        if (first != old_first)
            show_tiles(first);
        if (first != old_first || (keysDown() & KEY_A))
            print_status(first, total, pal);
    }
}
