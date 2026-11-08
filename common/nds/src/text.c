#include "text.h"

#include <string.h>

// FNT1 header: magic, u16 count, u8 ascent, u8 descent, 4 reserved.
#define FONT_HEADER 12

typedef struct {
    uint32_t cp;
    uint32_t offset;
    uint8_t w, h;
    int8_t xoff, yoff;
    uint8_t advance;
    uint8_t pad[3];
} Glyph;

static int font_count(const uint8_t *font)
{
    return font[4] | font[5] << 8;
}

int textAscent(const uint8_t *font)
{
    return font[6];
}

int textLineHeight(const uint8_t *font)
{
    return font[6] + font[7];
}

static const Glyph *find_glyph(const uint8_t *font, uint32_t cp)
{
    const Glyph *g = (const Glyph *)(font + FONT_HEADER);
    int lo = 0, hi = font_count(font) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g[mid].cp == cp)
            return &g[mid];
        if (g[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

// Glyph for a code point, or '?' if the font does not have it.
static const Glyph *glyph(const uint8_t *font, uint32_t cp)
{
    const Glyph *g = find_glyph(font, cp);
    return g ? g : find_glyph(font, '?');
}

static uint32_t next_cp(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t cp = *s++;
    int more = 0;
    if (cp >= 0xF0) {
        cp &= 0x07;
        more = 3;
    } else if (cp >= 0xE0) {
        cp &= 0x0F;
        more = 2;
    } else if (cp >= 0xC0) {
        cp &= 0x1F;
        more = 1;
    }
    while (more-- && (*s & 0xC0) == 0x80)
        cp = cp << 6 | (*s++ & 0x3F);
    *ps = (const char *)s;
    return cp;
}

void textCanvasInit(TextCanvas *c, uint16_t *map, uint32_t *gfx, int first_tile, int pal,
                    int w, int h)
{
    c->gfx = gfx;
    c->w = w;
    c->h = h;
    for (int ty = 0; ty < h; ty++)
        for (int tx = 0; tx < w; tx++)
            map[ty * 32 + tx] = (first_tile + ty * w + tx) | pal << 12;
    // VRAM takes 16/32-bit writes only, so no memset.
    for (int i = 0; i < w * h * 8; i++)
        gfx[i] = 0;
}

static inline void plot(TextCanvas *c, int x, int y, int colour)
{
    if ((unsigned)x >= (unsigned)(c->w * 8) || (unsigned)y >= (unsigned)(c->h * 8))
        return;
    uint32_t *row = &c->gfx[((y >> 3) * c->w + (x >> 3)) * 8 + (y & 7)];
    int shift = (x & 7) * 4;
    *row = (*row & ~(0xFu << shift)) | (uint32_t)colour << shift;
}

void textFill(TextCanvas *c, int x, int y, int w, int h, int colour)
{
    // Clip, then fill whole 4-pixel nibble groups per tile row word.
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > c->w * 8)
        w = c->w * 8 - x;
    if (y + h > c->h * 8)
        h = c->h * 8 - y;
    if (w <= 0 || h <= 0)
        return;
    uint32_t fill = (uint32_t)colour * 0x11111111u;
    int tx0 = x >> 3, tx1 = (x + w - 1) >> 3;
    for (int j = y; j < y + h; j++) {
        uint32_t *row = &c->gfx[((j >> 3) * c->w) * 8 + (j & 7)];
        for (int tx = tx0; tx <= tx1; tx++) {
            int a = tx == tx0 ? x & 7 : 0;
            int b = tx == tx1 ? ((x + w - 1) & 7) + 1 : 8;
            uint32_t mask = (b == 8 ? 0xFFFFFFFFu : (1u << (b * 4)) - 1) & ~((1u << (a * 4)) - 1);
            uint32_t *p = &row[tx * 8];
            *p = (*p & ~mask) | (fill & mask);
        }
    }
}

void textPutRow(TextCanvas *c, int x, int y, const uint8_t *colours, int n)
{
    if ((unsigned)y >= (unsigned)(c->h * 8) || x < 0 || x + n > c->w * 8)
        return;
    uint32_t *row = &c->gfx[((y >> 3) * c->w) * 8 + (y & 7)];
    while (n > 0) {
        uint32_t *p = &row[(x >> 3) * 8];
        int a = x & 7, b = a + n < 8 ? a + n : 8;
        uint32_t word = 0, mask = 0;
        for (int i = a; i < b; i++) {
            word |= (uint32_t)*colours++ << (i * 4);
            mask |= 0xFu << (i * 4);
        }
        *p = mask == 0xFFFFFFFFu ? word : (*p & ~mask) | word;
        n -= b - a;
        x += b - a;
    }
}

// Merge 8 pixels (nibbles of `word`, where `mask` is set) into row y at x.
static void put8(TextCanvas *c, int x, int y, uint32_t word, uint32_t mask)
{
    if ((unsigned)y >= (unsigned)(c->h * 8) || x < 0 || x + 8 > c->w * 8)
        return;
    uint32_t *row = &c->gfx[((y >> 3) * c->w + (x >> 3)) * 8 + (y & 7)];
    int s = (x & 7) * 4;
    if (!s) {
        *row = (*row & ~mask) | word;
        return;
    }
    row[0] = (row[0] & ~(mask << s)) | word << s;
    row[8] = (row[8] & ~(mask >> (32 - s))) | word >> (32 - s);
}

void textBlitNes(TextCanvas *c, const uint8_t *tile, int x, int y, const uint8_t colours[4])
{
    for (int r = 0; r < 8; r++) {
        uint8_t lo = tile[r], hi = tile[r + 8];
        uint32_t word = 0, mask = 0;
        for (int i = 0; i < 8; i++) {
            int v = (lo >> (7 - i) & 1) | (hi >> (7 - i) & 1) << 1;
            if (colours[v]) {
                word |= (uint32_t)colours[v] << (i * 4);
                mask |= 0xFu << (i * 4);
            }
        }
        if (mask)
            put8(c, x, y + r, word, mask);
    }
}

int textWidth(const uint8_t *font, const char *s)
{
    int w = 0;
    while (*s) {
        const Glyph *g = glyph(font, next_cp(&s));
        if (g)
            w += g->advance;
    }
    return w;
}

int textDraw(TextCanvas *c, const uint8_t *font, int x, int y, const char *s, int colour)
{
    int baseline = y + textAscent(font);
    while (*s) {
        const Glyph *g = glyph(font, next_cp(&s));
        if (!g)
            continue;
        const uint8_t *bits = font + g->offset;
        int stride = (g->w + 7) / 8;
        int gx = x + g->xoff, gy = baseline - g->yoff - g->h;
        for (int r = 0; r < g->h; r++)
            for (int col = 0; col < g->w; col++)
                if (bits[r * stride + col / 8] & (0x80 >> (col & 7)))
                    plot(c, gx + col, gy + r, colour);
        x += g->advance;
    }
    return x;
}

int textMessageCount(const uint8_t *txt)
{
    return txt[4] | txt[5] << 8;
}

const uint8_t *textMessage(const uint8_t *txt, int index, int *lines)
{
    const uint8_t *o = txt + 8 + index * 4;
    const uint8_t *m = txt + (o[0] | o[1] << 8 | o[2] << 16 | o[3] << 24);
    *lines = m[0];
    return m + 1;
}

const uint8_t *textNextLine(const uint8_t *line)
{
    return line + 2 + strlen((const char *)line + 2) + 1;
}
