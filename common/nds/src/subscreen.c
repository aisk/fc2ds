#include "subscreen.h"

#include <nds.h>

static TextCanvas canvas;
static int ready;

TextCanvas *subscreenInit(void)
{
    if (!ready) {
        ready = 1;
        videoSetModeSub(REG_DISPCNT_SUB | DISPLAY_BG1_ACTIVE);
        int bg = bgInitSub(1, BgType_Text4bpp, BgSize_T_256x256, 4, 3);
        bgSetPriority(bg, 0);
        textCanvasInit(&canvas, bgGetMapPtr(bg), (uint32_t *)bgGetGfxPtr(bg), 0, SUBSCREEN_PAL,
                       32, 24);
    }
    return &canvas;
}

void subscreenColour(int index, uint16_t rgb)
{
    BG_PALETTE_SUB[SUBSCREEN_PAL * 16 + index] = rgb;
}

int subscreenTouched(int *x, int *y)
{
    if (!(keysDown() & KEY_TOUCH))
        return 0;
    touchPosition t;
    touchRead(&t);
    *x = t.px;
    *y = t.py;
    return 1;
}
