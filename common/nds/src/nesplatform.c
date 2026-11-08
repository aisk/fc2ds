#include "nesplatform.h"

#include <maxmod9.h>
#include <nds.h>

#include "ndsvideo.h"
#include "vapu.h"
#include "vnes.h"
#include "vraster.h"

static Vnes nes;
static NesPlatform platform;
static VrasterFrame frame;

// Sound: the APU writes go straight to vapu, and a maxmod stream in manual
// mode asks for the samples it has played once per frame (present), so the
// synthesis runs on the main thread between the frames' register writes.
#define SOUND_RATE 32768

static mm_word sound_fill(mm_word length, mm_addr dest, mm_stream_formats format)
{
    (void)format;
    vapuRender(dest, (int)length);
    return length;
}

static void sound_init(void)
{
    mmInitNoSoundbank();
    vapuInit(SOUND_RATE);
    vnes->apu_write = vapuWrite;
    mm_stream stream = {
        .sampling_rate = SOUND_RATE,
        .buffer_length = 2048,
        .callback = sound_fill,
        .format = MM_STREAM_16BIT_MONO,
        .timer = MM_TIMER0,
        .manual = true,
    };
    mmStreamOpen(&stream);
}

void nesPlatformRun(const NesPlatform *p)
{
    platform = *p;
    vnesInit(&nes, p->prg, p->prg_size, p->chr, p->chr_size);
    vnes = &nes;
    ndsVideoInit(p->chr, p->chr_size);
    sound_init();
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

// CPU load: timer 2 counts from one vblank to the point where the next
// frame is ready (33.5 MHz / 256, so an NDS frame is about 2188 ticks and
// the counter wraps after half a second).
// Holding L+R shows the busiest frame of the last second as the bottom
// screen's backdrop: green below half, yellow below 90%, red when frames
// are dropped.
#define FRAME_TICKS 2188

static uint16_t load_max, load_shown;
static int load_frames;

static void show_load(void)
{
    if ((keysHeld() & (KEY_L | KEY_R)) != (KEY_L | KEY_R))
        return;
    uint16_t c = load_shown < FRAME_TICKS / 2   ? RGB15(0, 20, 0)
                 : load_shown < FRAME_TICKS * 9 / 10 ? RGB15(24, 24, 0)
                                                    : RGB15(28, 0, 0);
    BG_PALETTE_SUB[0] = c;
}

// The visible lines are done: show them at the next NDS vblank.
static void present(void)
{
    NdsLayout layout;
    vrasterBuild(&frame);
    GameScreenLayout(&frame, &layout);
    ndsVideoBuild(&frame, &layout);

    uint16_t busy = TIMER_DATA(2);
    if (busy > load_max)
        load_max = busy;
    if (++load_frames == 60) {
        load_shown = load_max;
        load_max = 0;
        load_frames = 0;
    }

    swiWaitForVBlank();
    TIMER_CR(2) = 0;
    TIMER_DATA(2) = 0;
    TIMER_CR(2) = TIMER_ENABLE | TIMER_DIV_256;
    ndsVideoCommit();
    show_load();
    mmStreamUpdate();
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
