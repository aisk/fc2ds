#include "nesplatform.h"

#include <nds.h>

#include "ndsvideo.h"
#include "vnes.h"
#include "vraster.h"

static Vnes nes;
static NesPlatform platform;
static VrasterFrame frame;

void nesPlatformRun(const NesPlatform *p)
{
    platform = *p;
    vnesInit(&nes, p->prg, p->prg_size, p->chr, p->chr_size);
    vnes = &nes;
    ndsVideoInit(p->chr, p->chr_size);
    p->main();
}

// NES joypad bits: A, B, Select, Start, Up, Down, Left, Right.
static uint8_t read_pad(void)
{
    scanKeys();
    uint32_t k = keysHeld();
    return (k & (KEY_A | KEY_X) ? 0x01 : 0) | (k & (KEY_B | KEY_Y) ? 0x02 : 0) | (k & KEY_SELECT ? 0x04 : 0) |
           (k & KEY_START ? 0x08 : 0) | (k & KEY_UP ? 0x10 : 0) | (k & KEY_DOWN ? 0x20 : 0) |
           (k & KEY_LEFT ? 0x40 : 0) | (k & KEY_RIGHT ? 0x80 : 0);
}

// The visible lines are done: show them at the next NDS vblank.
static void present(void)
{
    NdsLayout layout;
    vrasterBuild(&frame);
    GameScreenLayout(&frame, &layout);
    ndsVideoBuild(&frame, &layout);
    swiWaitForVBlank();
    ndsVideoCommit();
    vnes->pad = read_pad();
}

// Let NES time pass until the next NMI has been handled, taking the MMC3
// IRQs of the lines on the way and showing the frame when its last visible
// line is done. Game logic takes no NES time, so this is where the frame's
// 1/60 s goes.
void portWaitNmi(void)
{
    for (;;) {
        if (vnes->nmi) {
            vnes->nmi = 0;
            platform.nmi();
            return;
        }
        if (vnes->irq_pending) {
            platform.irq();
            continue;
        }
        int before = vnes->line;
        vnesAddCycles((int)((vnes->dots_per_line - vnes->dots + 2) / 3));
        if (before == 239 && vnes->line == 240)
            present();
    }
}

void portTickEnd(void)
{
}
