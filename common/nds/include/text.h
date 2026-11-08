// Proportional bitmap text drawn into a 4bpp tile canvas, plus access to the
// message tables. Fonts come from common/tools/mkfont.py ("FNT1"), messages
// from common/tools/mktext.py ("TXT1").
#pragma once

#include <stdbool.h>
#include <stdint.h>

// A rectangle of BG tiles where every map cell has its own tile, so text can
// be drawn at any pixel position.
typedef struct {
    uint32_t *gfx; // tile data of the first canvas tile
    int w, h;      // size in tiles
} TextCanvas;

// Point the map cells of a w x h tile area at consecutive tiles starting at
// first_tile (whose data is at gfx) and clear them. map is a 32-column map.
void textCanvasInit(TextCanvas *c, uint16_t *map, uint32_t *gfx, int first_tile, int pal,
                    int w, int h);

// Fill a pixel rectangle with one colour index (0 = transparent).
void textFill(TextCanvas *c, int x, int y, int w, int h, int colour);

// Set n pixels of row y from x on to the given colours.
void textPutRow(TextCanvas *c, int x, int y, const uint8_t *colours, int n);

// Draw a NES 2bpp tile (16 bytes) at (x, y), NES colour v as canvas colour
// colours[v]; 0 leaves the pixel as it is.
void textBlitNes(TextCanvas *c, const uint8_t *tile, int x, int y, const uint8_t colours[4]);

// Line height and baseline of a font in pixels.
int textLineHeight(const uint8_t *font);
int textAscent(const uint8_t *font);

// Width in pixels of a UTF-8 string.
int textWidth(const uint8_t *font, const char *s);

// Draw a UTF-8 string with its line box top-left at (x, y). Returns the x
// position after the last character.
int textDraw(TextCanvas *c, const uint8_t *font, int x, int y, const char *s, int colour);

// Messages: count, and the line count / lines of one message. A line is
// "u8 x, u8 y, UTF-8 text, 0"; textNextLine steps to the following one.
int textMessageCount(const uint8_t *txt);
const uint8_t *textMessage(const uint8_t *txt, int index, int *lines);
const uint8_t *textNextLine(const uint8_t *line);
