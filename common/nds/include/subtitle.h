// Translated subtitles on the bottom screen: the game keeps drawing its own
// (Japanese) text, and the platform shows the same message from a
// translation table (common/tools/mktext.py) in a band of the bottom screen.
// Uses sub engine BG1 (map base 4, tile base 3) and BG palette 15.
#pragma once

#include <stdint.h>

typedef struct {
    const char *name; // shown when switching, e.g. "English"
    const uint8_t *text, *font;
} SubtitleLang;

// Languages to cycle through by touching the bottom screen (subtitles off
// is always one of the choices), the one to start with (-1 = off) and the
// pixel rows of the bottom screen to draw in.
void subtitleInit(const SubtitleLang *langs, int count, int start, int y, int h);

// Show message `msg` of the table, or hide the subtitle (msg < 0).
void subtitleShow(int msg);

// Once per frame, during vblank: touch input and redrawing.
void subtitleFrame(void);
