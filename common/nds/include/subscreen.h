// The bottom screen canvas: sub engine BG1 (map base 4, tile base 3), a
// 256x192 pixel canvas above the NES picture, in BG palette 15. Subtitles
// (subtitle.h) and the game's own bottom screen (NesPlatform.frame) draw
// into it. Colour 0 is transparent; 1-3 belong to the subtitles and 4-15
// are free for the game.
#pragma once

#include <stdint.h>

#include "text.h"

#define SUBSCREEN_PAL 15

// Set up BG1 and clear the canvas (once; later calls only return it).
TextCanvas *subscreenInit(void);

// Set canvas colour `index` (1-15) to an RGB15 value.
void subscreenColour(int index, uint16_t rgb);

// The point touched this frame (keysDown), if any.
int subscreenTouched(int *x, int *y);
